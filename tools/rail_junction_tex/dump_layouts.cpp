// Writes the canonical rail junction layouts (src/city/rail_junction_plane.h JunctionLayouts) as JSON for the SPOM baker
// (gen_junction_tex.py): the plane rect and texture size, every route's centre line (fine), the tie stations (plain /
// long timber / covered, the same TieStations the meshes use), frogs and check rails, and the level crossing frame.
// So the baked picture and the C++ graph / props agree to the centimetre. Run: dump_layouts.bat
#include <cstdio>

#include "city/rail_junction_plane.h"

using namespace tbd::city;

int main(int argc, char** argv) {
    FILE* f = argc > 1 ? std::fopen(argv[1], "wb") : stdout;
    if (!f) return 1;
    const auto& L = JunctionLayouts();
    std::fprintf(f, "{\"texel\": %.4f, \"pitch\": %.4f, \"railTop\": %.4f, \"layouts\": [\n", kJunctionTexel, jmesh::kTieStep, jmesh::kRailTop);
    for (size_t i = 0; i < L.size(); ++i) {
        const JunctionParams p = AdjustParams(L[i].p);
        std::vector<char> virt;
        const auto routes = JunctionRoutes(p, &virt);
        const PlaneRect r = JunctionPlaneRect(p);
        int nA, nX;
        LayoutTexSize(r, nA, nX);
        const bool level = p.type == JType::LevelCrossing;
        const jmesh::CrossingFrame F = jmesh::CrossFrame(p);
        const float roadHalf = p.roadWidth * 0.5f + p.sidewalk + 0.2f;
        auto skip = [&](V2 q) { return level && std::fabs(Dot2(q, F.nr)) < roadHalf; };
        const auto st = jmesh::TieStations(routes, virt, p.gauge, skip);
        const jmesh::FrogPlan fr = jmesh::PlanFrogs(routes, virt, p.gauge);
        std::fprintf(f, "{\"id\": \"%s\", \"type\": \"%s\", \"angle\": %.3f, \"radius\": %.3f, \"gauge\": %.4f,\n", L[i].id.c_str(), JTypeName(p.type), p.angleDeg, p.radius, p.gauge);
        std::fprintf(f, " \"rect\": [%.4f, %.4f, %.4f, %.4f], \"tex\": [%d, %d],\n \"routes\": [", r.x0, r.z0, r.x1, r.z1, nA, nX);
        for (size_t k = 0; k < routes.size(); ++k) {
            std::fprintf(f, "%s{\"virt\": %d, \"pts\": [", k ? ",\n  " : "", (int)virt[k]);
            for (size_t j = 0; j < routes[k].size(); ++j) {
                const auto& q = routes[k][j];
                std::fprintf(f, "%s[%.4f, %.4f, %.5f, %.5f, %.4f]", j ? ", " : "", q.c.x, q.c.z, q.f.x, q.f.z, q.s);
            }
            std::fprintf(f, "], \"ties\": [");
            for (size_t j = 0; j < st[k].size(); ++j) {
                const auto& t = st[k][j];
                std::fprintf(f, "%s[%.4f, %.4f, %.4f, %.5f, %.5f, %d, %.4f, %.4f]", j ? ", " : "", t.s, t.c.x, t.c.z, t.f.x, t.f.z, (int)t.state, t.lo, t.hi);
            }
            std::fprintf(f, "]}");
        }
        std::fprintf(f, "],\n \"frogs\": [");
        for (size_t j = 0; j < fr.frogs.size(); ++j) std::fprintf(f, "%s[%.4f, %.4f, %.5f, %.5f]", j ? ", " : "", fr.frogs[j].p.x, fr.frogs[j].p.z, fr.frogs[j].dir.x, fr.frogs[j].dir.z);
        std::fprintf(f, "],\n \"checks\": [");
        for (size_t j = 0; j < fr.checks.size(); ++j) std::fprintf(f, "%s[%.4f, %.4f, %.5f, %.5f]", j ? ", " : "", fr.checks[j].c.x, fr.checks[j].c.z, fr.checks[j].f.x, fr.checks[j].f.z);
        std::fprintf(f, "],\n \"level\": ");
        if (level) std::fprintf(f, "{\"r\": [%.5f, %.5f], \"half\": %.3f, \"outer\": %.3f, \"deckHalf\": %.3f, \"flange\": %.4f}", F.r.x, F.r.z, F.half, F.outer, jmesh::kDeckHalf, jmesh::kFlange);
        else std::fprintf(f, "null");
        std::fprintf(f, "}%s\n", i + 1 < L.size() ? "," : "");
    }
    std::fprintf(f, "]}\n");
    if (f != stdout) std::fclose(f);
    std::printf("%zu layouts\n", L.size());
    return 0;
}
