// Tests for src/terrain_field.h and its use in Terrain (HeightAt = procedural base + player edits). No renderer is created.
#include <cmath>
#include <cstdio>
#include <vector>

#include "../src/terrain.h"

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)
static bool Near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

using tbd::HeightField;
static float Flat(float, float) { return 5.0f; }

static void EmptyFieldCostsNothing() {
    HeightField f;
    CHECK(f.Empty());
    CHECK(f.EditAt(0, 0) == 0.0f);
    CHECK(f.EditAt(-9999.0f, 9999.0f) == 0.0f);
    CHECK(f.EditAt(1e9f, -1e9f) == 0.0f); // far outside the map
    CHECK(f.ResidentBytes() == 0);
    CHECK(f.VersionInRect(-100, -100, 100, 100) == 0);
}

static void RaiseAndLower() {
    HeightField f;
    f.ApplyBrush(100.0f, 200.0f, 30.0f, 2.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    CHECK(Near(f.EditAt(100.0f, 200.0f), 2.0f, 0.03f));   // centre gets the full strength (to 2 cm quantisation)
    CHECK(f.EditAt(100.0f, 200.0f) > f.EditAt(115.0f, 200.0f));
    CHECK(f.EditAt(115.0f, 200.0f) > f.EditAt(125.0f, 200.0f));
    CHECK(f.EditAt(131.0f, 200.0f) == 0.0f);               // outside the radius
    CHECK(Near(f.EditAt(120.0f, 200.0f), f.EditAt(80.0f, 200.0f), 0.03f)); // symmetric
    f.ApplyBrush(100.0f, 200.0f, 30.0f, -2.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    CHECK(Near(f.EditAt(100.0f, 200.0f), 0.0f, 0.03f));    // digging undoes it
}

static void SmoothAcrossTileBorders() {
    // A brush centred on a tile border: the field must have no jump when sampled across it.
    HeightField f;
    const float border = -10000.0f + HeightField::kTileMetres * 20; // an edge between two tiles
    f.ApplyBrush(border, 0.0f, 40.0f, 3.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    CHECK(f.TileCount() >= 2);
    float worst = 0.0f;
    for (float x = border - 3.0f; x < border + 3.0f; x += 0.1f) worst = std::fmax(worst, std::fabs(f.EditAt(x + 0.1f, 0.0f) - f.EditAt(x, 0.0f)));
    CHECK(worst < 0.1f); // a 0.1 m step moves the surface by well under 10 cm
}

static void FlattenReachesTarget() {
    HeightField f;
    auto base = [](float x, float) { return 0.1f * x; }; // a slope
    for (int i = 0; i < 40; ++i) f.ApplyBrush(500.0f, 500.0f, 20.0f, 1.0f, HeightField::BrushMode::Flatten, 70.0f, base);
    CHECK(Near(base(500.0f, 500.0f) + f.EditAt(500.0f, 500.0f), 70.0f, 0.05f));
    CHECK(Near(base(505.0f, 500.0f) + f.EditAt(505.0f, 500.0f), 70.0f, 1.0f)); // near the centre it also flattened
}

static void SaturatesInsteadOfWrapping() {
    HeightField f;
    for (int i = 0; i < 400; ++i) f.ApplyBrush(0.0f, 0.0f, 10.0f, 50.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    const float top = f.EditAt(0.0f, 0.0f);
    CHECK(top > 600.0f && top < 700.0f); // clamped near +640 m, not wrapped negative
}

static void OutsideTheMapIsIgnored() {
    HeightField f;
    f.ApplyBrush(50000.0f, 0.0f, 100.0f, 5.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    CHECK(f.Empty());
    f.ApplyBrush(-10005.0f, 0.0f, 20.0f, 5.0f, HeightField::BrushMode::Add, 0.0f, Flat); // overlaps the map edge
    CHECK(!f.Empty());
}

static void DirtyAndVersions() {
    HeightField f;
    const uint32_t v0 = f.VersionInRect(-50, -50, 50, 50);
    f.ApplyBrush(0.0f, 0.0f, 20.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    const auto dirty = f.TakeDirty();
    CHECK(!dirty.empty());
    CHECK(f.TakeDirty().empty()); // taken once
    const uint32_t v1 = f.VersionInRect(-50, -50, 50, 50);
    CHECK(v1 != v0);
    CHECK(f.VersionInRect(5000, 5000, 5100, 5100) == 0); // far tiles unaffected
    f.ApplyBrush(0.0f, 0.0f, 20.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    CHECK(f.VersionInRect(-50, -50, 50, 50) != v1);
}


static void VersionsFollowSixtyFourMetreBlocks() {
    HeightField f;
    f.ApplyBrush(0.0f, 0.0f, 20.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat); // sits on a block corner: touches the four around it
    CHECK(f.VersionInRect(0.0f, 0.0f, 63.99f, 63.99f) != 0);
    CHECK(f.VersionInRect(-64.0f, -64.0f, -0.01f, -0.01f) != 0);
    CHECK(f.VersionInRect(64.0f, 0.0f, 127.99f, 63.99f) == 0);   // the next block east is untouched
    CHECK(f.VersionInRect(0.0f, 192.0f, 63.99f, 255.99f) == 0);  // and one far north
    const uint32_t before = f.VersionInRect(64.0f, 0.0f, 127.99f, 63.99f);
    f.ApplyBrush(30.0f, 30.0f, 10.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat); // reaches x = 40 only: still inside block 0
    CHECK(f.VersionInRect(64.0f, 0.0f, 127.99f, 63.99f) == before);
    f.ApplyBrush(61.0f, 30.0f, 4.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat); // within 2 samples of the seam at x = 64
    CHECK(f.VersionInRect(64.0f, 0.0f, 127.99f, 63.99f) != before); // the neighbour's edge normals changed too
    std::vector<uint8_t> bytes;
    f.Save(bytes);
    HeightField g;
    const uint32_t v0 = g.VersionInRect(0.0f, 0.0f, 63.99f, 63.99f);
    CHECK(g.Load(bytes.data(), bytes.size()));
    CHECK(g.VersionInRect(0.0f, 0.0f, 63.99f, 63.99f) != v0); // loading makes everything rebuild once
}

static void CompactDropsUndoneTiles() {
    HeightField f;
    f.ApplyBrush(0.0f, 0.0f, 20.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    f.ApplyBrush(0.0f, 0.0f, 20.0f, -1.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    CHECK(f.Compact() >= 1);
    CHECK(f.Empty());
}

static void SaveLoadRoundTrip() {
    HeightField a;
    a.ApplyBrush(300.0f, -400.0f, 60.0f, 4.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    a.ApplyBrush(-2000.0f, 1500.0f, 25.0f, -2.5f, HeightField::BrushMode::Add, 0.0f, Flat);
    std::vector<uint8_t> bytes;
    a.Save(bytes);
    CHECK(bytes.size() > 12);
    HeightField b;
    CHECK(b.Load(bytes.data(), bytes.size()));
    bool same = true;
    for (float x = -2100.0f; x < 400.0f; x += 7.3f)
        for (float z = -450.0f; z < 1600.0f; z += 9.1f)
            if (a.EditAt(x, z) != b.EditAt(x, z)) same = false;
    CHECK(same);
    CHECK(a.TileCount() == b.TileCount());
    // Damaged data is refused and leaves the field as it was.
    HeightField c;
    c.ApplyBrush(0.0f, 0.0f, 10.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    const float before = c.EditAt(0.0f, 0.0f);
    CHECK(!c.Load(bytes.data(), 7));
    std::vector<uint8_t> cut(bytes.begin(), bytes.end() - 5);
    CHECK(!c.Load(cut.data(), cut.size()));
    std::vector<uint8_t> wrong = bytes; wrong[0] = 'X';
    CHECK(!c.Load(wrong.data(), wrong.size()));
    CHECK(c.EditAt(0.0f, 0.0f) == before);
}

static void SmallEditsSaveSmall() {
    HeightField f;
    f.ApplyBrush(0.0f, 0.0f, 30.0f, 1.0f, HeightField::BrushMode::Add, 0.0f, Flat);
    std::vector<uint8_t> bytes;
    f.Save(bytes);
    CHECK(bytes.size() < f.ResidentBytes() / 4); // a lone hill is far smaller on disk than its tiles in memory
}

static void TerrainUsesTheField() {
    tbd::Terrain t;
    const float x = 400.0f, z = -250.0f;
    const float base = t.BaseHeightAt(x, z);
    CHECK(t.HeightAt(x, z) == base); // no edits: identical to before
    t.Brush(x, z, 30.0f, 6.0f);
    CHECK(Near(t.HeightAt(x, z), base + 6.0f, 0.05f));
    CHECK(t.BaseHeightAt(x, z) == base); // the base is never changed
    t.Brush(x, z, 30.0f, -6.0f);
    CHECK(Near(t.HeightAt(x, z), base, 0.05f));
    // Raycast sees the raised ground.
    t.Brush(x, z, 30.0f, 20.0f);
    tbd::Ray ray;
    ray.origin = {x, t.BaseHeightAt(x, z) + 60.0f, z};
    ray.dir = {0.0f, -1.0f, 0.0f};
    tbd::Vec3 hit{};
    CHECK(t.Raycast(ray, 200.0f, hit));
    CHECK(Near(hit.y, t.BaseHeightAt(x, z) + 20.0f, 0.2f));
}

int main() {
    EmptyFieldCostsNothing();
    RaiseAndLower();
    SmoothAcrossTileBorders();
    FlattenReachesTarget();
    SaturatesInsteadOfWrapping();
    OutsideTheMapIsIgnored();
    DirtyAndVersions();
    VersionsFollowSixtyFourMetreBlocks();
    CompactDropsUndoneTiles();
    SaveLoadRoundTrip();
    SmallEditsSaveSmall();
    TerrainUsesTheField();
    std::printf("%d checks, %d failed\n", g_checks, g_failed);
    return g_failed == 0 ? 0 : 1;
}
