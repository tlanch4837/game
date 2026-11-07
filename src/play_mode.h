// Play mode: the toolbar's Play button hands the viewport to a first-person free-fly camera (WASD, Space / Ctrl for up /
// down, Shift to go fast, the wheel for speed, the mouse to look). G toggles walking, which keeps the eye 1.7 m above the
// ground. Esc or Stop returns to the editor; main.cpp restores the sun, this class restores the camera and the mouse.
//
// Ground for walking: the imported terrain pieces of the scene (files named map_*.glb, not the border or the water) are
// read once on a worker thread into a coarse grid of the highest surface height per cell, and a downward ray at (x, z)
// becomes a bilinear lookup in that grid. See GroundGrid. Nothing here touches the renderer.
#pragma once
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "camera.h"
#include "platform/platform.h"
#include "core/jobs.h"
#include "render/renderer.h"
#include "scene.h"
#include "vecmath.h"

namespace tbd {

constexpr float kPlayEyeHeight = 1.7f;

// Highest surface height per cell of a regular grid over the terrain's footprint. A cell is empty where no triangle covers
// its corner (holes in the map), and a lookup there is skipped, so the ground under a hole is whatever the neighbours say.
class GroundGrid {
public:
    static constexpr float kEmpty = -3.0e38f;
    enum State { Building = 1, Ready = 2, Failed = 3 };
    std::atomic<int> state{Building}; // written once by the worker with release, read by the main thread with acquire

    // Height of the surface under (x, z). False outside the grid or over a patch with no surface.
    bool Height(float x, float z, float& out) const {
        const float fx = (x - x0_) / cell_, fz = (z - z0_) / cell_;
        if (!(fx >= 0.0f && fz >= 0.0f && fx < (float)(nx_ - 1) && fz < (float)(nz_ - 1))) return false;
        const int i = (int)fx, j = (int)fz;
        const float tx = fx - (float)i, tz = fz - (float)j;
        const float w[4] = {(1 - tx) * (1 - tz), tx * (1 - tz), (1 - tx) * tz, tx * tz};
        const float h[4] = {At(i, j), At(i + 1, j), At(i, j + 1), At(i + 1, j + 1)};
        float sum = 0.0f, weight = 0.0f;
        for (int k = 0; k < 4; ++k)
            if (h[k] > kEmpty) { sum += h[k] * w[k]; weight += w[k]; }
        if (weight < 1e-4f) return false;
        out = sum / weight;
        return true;
    }

    struct Source { std::string path; Mat4 model; };
    using LoadFn = bool (*)(const std::string& path, std::vector<Vertex>& out); // unindexed triangles, safe on a worker

    // Worker thread. Imports every source, transforms it to world space and keeps the highest triangle per grid node.
    void Build(const std::vector<Source>& sources, LoadFn load) {
        std::vector<std::vector<Vec3>> worlds;
        Vec3 lo = {1e30f, 1e30f, 1e30f}, hi = {-1e30f, -1e30f, -1e30f};
        for (const Source& s : sources) {
            std::vector<Vertex> v;
            if (!load(s.path, v)) continue;
            std::vector<Vec3> w(v.size() - v.size() % 3);
            for (size_t i = 0; i < w.size(); ++i) {
                const Vec4 p = s.model * Vec4{v[i].px, v[i].py, v[i].pz, 1.0f};
                w[i] = {p.x, p.y, p.z};
                lo = {std::min(lo.x, p.x), 0, std::min(lo.z, p.z)};
                hi = {std::max(hi.x, p.x), 0, std::max(hi.z, p.z)};
            }
            worlds.push_back(std::move(w));
        }
        if (worlds.empty() || !(hi.x > lo.x) || !(hi.z > lo.z)) { state.store(Failed, std::memory_order_release); return; }

        const float extent = std::max(hi.x - lo.x, hi.z - lo.z);
        cell_ = std::max(2.0f, extent / 2048.0f); // at most about 2050 x 2050 nodes, 16 MB
        x0_ = lo.x;
        z0_ = lo.z;
        nx_ = (int)((hi.x - lo.x) / cell_) + 2;
        nz_ = (int)((hi.z - lo.z) / cell_) + 2;
        heights_.assign((size_t)nx_ * (size_t)nz_, kEmpty);
        for (const std::vector<Vec3>& w : worlds)
            for (size_t t = 0; t + 2 < w.size(); t += 3) Raster(w[t], w[t + 1], w[t + 2]);
        state.store(Ready, std::memory_order_release);
    }

private:
    float At(int i, int j) const { return heights_[(size_t)j * (size_t)nx_ + (size_t)i]; }
    void Raise(int i, int j, float y) {
        if (i < 0 || j < 0 || i >= nx_ || j >= nz_) return;
        float& h = heights_[(size_t)j * (size_t)nx_ + (size_t)i];
        h = std::max(h, y);
    }

    // The triangle's height at every grid node whose (x, z) falls inside it. A triangle too small to contain a node
    // raises the node nearest its centre, so a finely tessellated map leaves no holes.
    void Raster(const Vec3& a, const Vec3& b, const Vec3& c) {
        const float minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
        const float minZ = std::min({a.z, b.z, c.z}), maxZ = std::max({a.z, b.z, c.z});
        const int i0 = (int)std::ceil((minX - x0_) / cell_), i1 = (int)std::floor((maxX - x0_) / cell_);
        const int j0 = (int)std::ceil((minZ - z0_) / cell_), j1 = (int)std::floor((maxZ - z0_) / cell_);
        if (i0 > i1 || j0 > j1) {
            Raise((int)std::lround(((a.x + b.x + c.x) / 3.0f - x0_) / cell_), (int)std::lround(((a.z + b.z + c.z) / 3.0f - z0_) / cell_),
                  (a.y + b.y + c.y) / 3.0f);
            return;
        }
        const float d = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
        if (std::fabs(d) < 1e-12f) return; // edge-on to the sky
        const float inv = 1.0f / d;
        for (int j = std::max(j0, 0); j <= std::min(j1, nz_ - 1); ++j) {
            const float pz = z0_ + (float)j * cell_;
            for (int i = std::max(i0, 0); i <= std::min(i1, nx_ - 1); ++i) {
                const float px = x0_ + (float)i * cell_;
                const float l1 = ((b.z - c.z) * (px - c.x) + (c.x - b.x) * (pz - c.z)) * inv;
                const float l2 = ((c.z - a.z) * (px - c.x) + (a.x - c.x) * (pz - c.z)) * inv;
                const float l3 = 1.0f - l1 - l2;
                if (l1 >= -1e-4f && l2 >= -1e-4f && l3 >= -1e-4f) Raise(i, j, l1 * a.y + l2 * b.y + l3 * c.y);
            }
        }
    }

    int nx_ = 0, nz_ = 0;
    float x0_ = 0.0f, z0_ = 0.0f, cell_ = 1.0f;
    std::vector<float> heights_;
};

class PlayMode {
public:
    bool active = false;
    bool walking = false;
    bool player = false;   // a third-person character is being driven (player_character.h); G switches it and free fly
    bool scripted = false; // TBD_PLAYTEST: no mouse, no keys; the script drives
    float flySpeed = 20.0f;  // m/s
    float walkSpeed = 5.0f;  // m/s

    // The terrain files of a scene: map_*.glb, but not the map's border or water (their surfaces are not ground).
    static bool IsTerrainEntity(const Entity& e) {
        if (e.mesh != MeshKind::Imported) return false;
        const size_t cut = e.meshPath.find_last_of("\\/");
        std::string name = cut == std::string::npos ? e.meshPath : e.meshPath.substr(cut + 1);
        for (char& ch : name) ch = (char)std::tolower((unsigned char)ch);
        return name.rfind("map_", 0) == 0 && name.find("border") == std::string::npos && name.find("water") == std::string::npos;
    }

    // Starts reading the scene's terrain into a ground grid, unless the same terrain was already read (or is being read).
    void PrepareGround(JobSystem* jobs, const Scene& scene, GroundGrid::LoadFn load) {
        std::vector<GroundGrid::Source> sources;
        std::string signature;
        for (const Entity& e : scene.entities) {
            if (!IsTerrainEntity(e)) continue;
            sources.push_back({e.meshPath, e.Model()});
            char numbers[160];
            snprintf(numbers, sizeof(numbers), "|%g %g %g %g %g %g %g %g %g %g", e.position.x, e.position.y, e.position.z,
                     e.rotation.x, e.rotation.y, e.rotation.z, e.rotation.w, e.scale.x, e.scale.y, e.scale.z);
            signature += e.meshPath + numbers;
        }
        if (signature == signature_ && (sources.empty() || grid_)) return;
        signature_ = signature;
        grid_.reset();
        if (sources.empty() || !jobs) return;
        std::shared_ptr<GroundGrid> grid = std::make_shared<GroundGrid>();
        grid_ = grid;
        jobs->RunBackground([grid, sources, load] {
            try { grid->Build(sources, load); }
            catch (...) { grid->state.store(GroundGrid::Failed, std::memory_order_release); }
        });
    }

    // 0 = the scene has no imported terrain, else GroundGrid::Building / Ready / Failed.
    int GroundState() const { return grid_ ? grid_->state.load(std::memory_order_acquire) : 0; }

    bool GroundHeight(float x, float z, float& y) const {
        return GroundState() == GroundGrid::Ready && grid_->Height(x, z, y);
    }

    // Takes over the mouse and the camera. False when refused (a TBD_SHOT / TBD_CAPTURE run, or no viewport).
    bool Begin(HWND viewport, const OrbitCamera& camera) {
        if (active || !viewport) return false;
        scripted = GetEnvironmentVariableA("TBD_PLAYTEST", nullptr, 0) > 0;
        if (!scripted && (GetEnvironmentVariableA("TBD_SHOT", nullptr, 0) > 0 || GetEnvironmentVariableA("TBD_CAPTURE", nullptr, 0) > 0)) return false;
        viewport_ = viewport;
        saved_ = camera;
        walking = false;
        player = false;
        active = true;
        if (scripted) return true; // the mouse stays the owner's
        SetFocus(viewport);
        SetCapture(viewport);
        ShowCursor(FALSE);
        cursorHidden_ = true;
        const POINT centre = LockCursor();
        SetCursorPos(centre.x, centre.y); // so the first frame's look delta is zero, not the distance from the toolbar
        return true;
    }

    // Gives the mouse back and restores the camera from before Begin.
    void End(OrbitCamera& camera) {
        if (!active) return;
        active = false;
        walking = false;
        player = false;
        camera = saved_;
        if (scripted) return;
        ClipCursor(nullptr);
        if (cursorHidden_) ShowCursor(TRUE);
        cursorHidden_ = false;
        if (GetCapture() == viewport_) ReleaseCapture();
    }

    // The window lost the keyboard (alt-tab, a dialog): the caller should stop, so the mouse is never held hostage.
    bool Lost() const {
        return active && !scripted && GetActiveWindow() != GetAncestor(viewport_, GA_ROOT) && GetActiveWindow() != GetAncestor(viewport_, GA_ROOTOWNER);
    }

    // G. Returns a message to show, or nullptr.
    const char* ToggleWalk(OrbitCamera& camera, bool (*ground)(float, float, float&)) {
        if (walking) { walking = false; return "FLYING"; }
        const Vec3 eye = camera.Eye();
        float y = 0.0f;
        if (!ground(eye.x, eye.z, y)) return GroundState() == GroundGrid::Building ? "GROUND IS STILL LOADING" : "NO GROUND HERE";
        walking = true;
        PlaceEye(camera, {eye.x, y + kPlayEyeHeight, eye.z});
        return "WALKING";
    }

    void Wheel(float notches) {
        if (walking) walkSpeed = std::clamp(walkSpeed * std::pow(1.25f, notches), 0.5f, 60.0f);
        else flySpeed = std::clamp(flySpeed * std::pow(1.25f, notches), 1.0f, 3000.0f);
    }

    float Speed() const { return walking ? walkSpeed : flySpeed; }

    // The mouse's movement since the last call, in pixels; recentres the cursor.
    void PollMouse(float& dx, float& dy) {
        dx = dy = 0.0f;
        if (!active || scripted) return;
        if (GetCapture() != viewport_) SetCapture(viewport_); // a toolbar click still in flight can take it
        POINT centre = LockCursor();
        POINT p;
        if (GetCursorPos(&p)) {
            dx = (float)(p.x - centre.x);
            dy = (float)(p.y - centre.y);
            SetCursorPos(centre.x, centre.y);
        }
    }

    // Once per frame: mouse look, then movement.
    void Update(OrbitCamera& camera, float dt, bool (*ground)(float, float, float&)) {
        if (!active) return;
        float lookX, lookY;
        PollMouse(lookX, lookY);
        camera.Look(lookX, lookY);

        using PK = tbd::platform::Key; auto down = [](PK k) { return tbd::platform::KeyDownAsync(k); };
        const bool fast = down(PK::Shift);
        const float forward = (down(PK::W) ? 1.0f : 0.0f) - (down(PK::S) ? 1.0f : 0.0f);
        const float right = (down(PK::D) ? 1.0f : 0.0f) - (down(PK::A) ? 1.0f : 0.0f);
        if (!walking) {
            const float up = (down(PK::Space) ? 1.0f : 0.0f) - (down(PK::Control) ? 1.0f : 0.0f);
            const float step = flySpeed * (fast ? 4.0f : 1.0f) * dt;
            camera.Fly(forward * step, right * step, up * step);
            return;
        }
        const float step = walkSpeed * (fast ? 2.5f : 1.0f) * dt;
        Vec3 flat = camera.Forward();
        flat.y = 0.0f;
        flat = Normalize(flat);
        Vec3 eye = camera.Eye() + flat * (forward * step) + camera.Right() * (right * step);
        float y = 0.0f;
        if (ground(eye.x, eye.z, y)) eye.y += (y + kPlayEyeHeight - eye.y) * (1.0f - std::exp(-15.0f * dt)); // eases over steps
        PlaceEye(camera, eye);
    }

private:
    // Moves the eye to `eye`, keeping the view direction (the orbit camera's target sits `distance` in front of it).
    static void PlaceEye(OrbitCamera& c, Vec3 eye) {
        const float cp = std::cos(c.pitch);
        c.target = eye - Vec3{cp * std::sin(c.yaw), std::sin(c.pitch), cp * std::cos(c.yaw)} * c.distance;
    }

    // Keeps the cursor inside the viewport and returns its centre in screen pixels.
    POINT LockCursor() {
        RECT rc = {};
        GetClientRect(viewport_, &rc);
        POINT a = {rc.left, rc.top}, b = {rc.right, rc.bottom};
        ClientToScreen(viewport_, &a);
        ClientToScreen(viewport_, &b);
        const RECT screen = {a.x, a.y, b.x, b.y};
        ClipCursor(&screen);
        return POINT{(a.x + b.x) / 2, (a.y + b.y) / 2};
    }

    HWND viewport_ = nullptr;
    OrbitCamera saved_;
    bool cursorHidden_ = false;
    std::shared_ptr<GroundGrid> grid_;
    std::string signature_;
};

} // namespace tbd
