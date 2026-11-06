// Times the terrain functions behind a brush stroke on the island: height lookups, a mouse ray, a chunk's worth and a far ring's worth.
#include <chrono>
#include <cstdio>
#include "../src/terrain.h"
using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a) { return std::chrono::duration<double, std::milli>(Clock::now() - a).count(); }
int main() {
    tbd::Terrain t;
    t.params.mapKind = 1; t.params.flatStart = false;
    volatile float sink = 0;
    for (int i = 0; i < 1000; ++i) sink += t.HeightAt(i * 3.0f - 1500.0f, i * 2.0f - 1000.0f); // builds the river tables
    auto a = Clock::now(); float s = 0;
    const int N = 200000;
    for (int i = 0; i < N; ++i) s += t.HeightAt(-3000.0f + (i % 500) * 11.0f, -4000.0f + (i / 500) * 17.0f);
    const double per = Ms(a) * 1e6 / N; sink = s;
    printf("HeightAt island: %.0f ns/call\n", per);
    a = Clock::now();
    for (int i = 0; i < N; ++i) s += t.field.EditAt(100.0f + i * 0.01f, 50.0f);
    printf("EditAt (empty field): %.1f ns\n", Ms(a) * 1e6 / N);
    tbd::Ray ray; ray.origin = {0, 9000, 6000}; ray.dir = tbd::Normalize(tbd::Vec3{0, -1, -0.6f});
    tbd::Vec3 hit; a = Clock::now(); int hits = 0;
    for (int i = 0; i < 20; ++i) hits += t.Raycast(ray, 30000.0f, hit) ? 1 : 0;
    printf("Raycast from 9 km up (30 km max): %.2f ms each, hit=%d\n", Ms(a) / 20, hits);
    a = Clock::now();
    for (int i = 0; i < 1089 * 5; ++i) s += t.HeightAt(500.0f + (i % 33) * 2.0f, 500.0f + (i / 33 % 33) * 2.0f);
    printf("one near chunk (33x33 verts, 5 lookups each): %.2f ms\n", Ms(a));
    a = Clock::now();
    for (int i = 0; i < 97 * 97 * 5; ++i) s += t.HeightAt(-2000.0f + (i % 97) * 100.0f, -2000.0f + (i / 97 % 97) * 100.0f);
    printf("one far ring (97x97 verts, 5 lookups each): %.2f ms (x3 levels per rebuild pass)\n", Ms(a));
    t.Brush(0, 0, 30, 1.0f);
    a = Clock::now();
    for (int i = 0; i < N; ++i) s += t.HeightAt(-3000.0f + (i % 500) * 11.0f, -4000.0f + (i / 500) * 17.0f);
    printf("HeightAt with edits present: %.0f ns/call\n", Ms(a) * 1e6 / N);
    sink = s; return 0;
}
