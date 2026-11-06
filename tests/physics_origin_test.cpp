#define NOMINMAX
#include "../src/physics.h"
#include "../src/scene.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <windows.h>

static bool NoMesh(const std::string&, std::vector<tbd::Vertex>&) { return false; }
static bool Near(float a, float b) { return std::fabs(a - b) < 0.01f; }
int main() {
    using namespace tbd;
    JobSystem jobs;
    PhysicsWorld physics;
    Scene scene;
    scene.entities.clear();
    Entity ground;
    ground.position = {20000, -0.5f, 0};
    ground.scale = {20, 1, 20};
    scene.entities.push_back(ground);
    physics.SetScene(&jobs, scene, L"", NoMesh);
    for (int i = 0; i < 500 && !physics.ReadyAround({20000, 0, 0}, 10); ++i) {
        physics.Update({20000, 0, 0}, 0);
        Sleep(1);
    }
    assert(physics.ReadyAround({20000, 0, 0}, 10));
    RayHit hit;
    assert(physics.Raycast({20000, 5, 0}, {0, -1, 0}, 10, hit));
    assert(Near(hit.point.y, 0));
    physics.CreateCharacter({20000, 0.05f, 0});
    const uint32_t ball = physics.AddBall({20002, 5, 0}, 0.25f);
    assert(ball != 0);
    physics.ShiftOrigin({20000, 0, 0});
    assert(physics.ReadyAround({0, 0, 0}, 10));
    assert(physics.Raycast({0, 5, 0}, {0, -1, 0}, 10, hit));
    assert(Near(hit.point.y, 0));
    assert(Near(physics.Character().feet.x, 0));
    assert(Near(physics.BallPosition(ball).x, 2));
    physics.Update({0, 0, 0}, 1.0f / 60.0f);
    assert(physics.BallPosition(ball).y < 5);
    physics.Update({1000, 0, 0}, 0);
    assert(!physics.Raycast({0, 5, 0}, {0, -1, 0}, 10, hit));
    physics.Update({0, 0, 0}, 0);
    assert(physics.ReadyAround({0, 0, 0}, 10));
    assert(physics.Raycast({0, 5, 0}, {0, -1, 0}, 10, hit));
    physics.ShiftOrigin({-100, 0, 0});
    assert(physics.Raycast({100, 5, 0}, {0, -1, 0}, 10, hit));
    assert(Near(physics.Character().feet.x, 100));
    std::puts("physics origin shift: collision, character, ball and streaming passed");
}
