// TBD_GUI_PREVIEW_DUMP=<file.js>: writes the procedural build items (the transport buildings without a GLB, power lines,
// pipes) as triangles for tools/gui_atlas/render.html, which renders every Glass Dock tile preview with one camera.
// Same format as industry_dump (city_industry.inl): plain triangles x y z r g b; slab triangles x y z u v per texture.
// Included by game_core.inl after the city .inl files (it uses their builders); runs once, on the first GUI frame.
#pragma once

bool GuiPreviewDump(const std::string& path, std::string& out) {
    using tbd::Vec3;
    using namespace tbd::city;
    std::string js = "window.GUI_MODELS=[\n";
    auto emit = [&](const std::string& name, const std::vector<tbd::Vertex>& v) {
        float minY = 1e30f;
        for (const tbd::Vertex& p : v) minY = std::min(minY, p.py);
        std::string plain, slab[tmodel::kSlabTexCount];
        char b[200];
        for (const tbd::Vertex& p : v) {
            int s = -1;
            for (int k = 0; k < tmodel::kSlabTexCount; ++k)
                if (std::fabs(p.a - tmodel::kSlabTex[k].alpha) < 0.0009f) s = k;
            if (s < 0) {
                snprintf(b, sizeof(b), "%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,", p.px, p.py - minY, p.pz, std::clamp(p.r, 0.0f, 1.0f), std::clamp(p.g, 0.0f, 1.0f), std::clamp(p.b, 0.0f, 1.0f));
                plain += b;
            } else {
                const bool roof = p.a > 0.815f;
                snprintf(b, sizeof(b), "%.2f,%.2f,%.2f,%.4f,%.4f,", p.px, p.py - minY, p.pz, p.u / std::max(p.g, 0.1f),
                         roof ? 1.0f - p.v / std::max(p.b, 0.1f) : p.v / std::max(p.b, 0.1f));
                slab[s] += b;
            }
        }
        js += "{name:\"" + name + "\",plain:[" + plain + "],slab:{";
        for (int k = 0; k < tmodel::kSlabTexCount; ++k)
            if (!slab[k].empty()) js += std::string("\"") + tmodel::kSlabTex[k].file + "\":[" + slab[k] + "],";
        js += "}},\n";
    };
    std::vector<tbd::Vertex> v;
    const auto& defs = Defs();
    for (size_t i = 0; i < defs.size(); ++i) {
        const BuildingDef& d = defs[i];
        if (d.glb) continue; // the GLB is rendered as it is
        v.clear();
        Placed p;
        p.def = (int)i, p.c = {0, 0}, p.front = {0, 1}, p.y = 0, p.state = BState::Done;
        tmodel::BuildPlaced(p, 0.0f, v, nullptr);
        emit(d.name, v);
    }
    // the lines the utilities tool lays: 120 m of wooden poles, 240 m of pylons, a pipe of each kind
    UtilLine l;
    l.kind = Util::Electricity, l.a = {-60, 0}, l.b = {60, 0}, l.elev = 0;
    v.clear(), UtilBuildPoles(l, v), emit("Electricity Line", v);
    l.a = {-120, 0}, l.b = {120, 0}, l.elev = 20;
    v.clear(), UtilBuildPylons(l, v), emit("Electricity Line (pylons)", v);
    for (int k = 1; k <= 2; ++k) {
        v.clear();
        const Vec3 col = ufx::Kind((Util)k, true);
        zfx::Beam(v, {-16, 0.8f, 0}, {16, 0.8f, 0}, 1.4f, col);
        for (float x : {-15.5f, 15.5f}) zfx::Beam(v, {x, 0.0f, 0}, {x, 1.9f, 0}, 1.9f, col * 0.8f); // flanges
        emit(k == 1 ? "Water Pipe" : "Sewage Pipe", v);
    }
    // the transit line tiles without a vehicle GLB: the airport's jet, the harbour's ferry, a container ship, a tram
    {
        v.clear();
        tmodel::Mesher m(v, nullptr, {0, 0}, {0, 1}, 0);
        tmodel::Plane(m, 0, 0, 38.0f, true);
        emit("Plane Line", v);
        v.clear();
        tmodel::HullZ(m, -7, 7, -19, 19, 0.0f, 5.2f, tmodel::kWhite);
        m.Box(-5.5f, 5.5f, -13, 9, 5.2f, 8.6f, {0.95f, 0.95f, 0.97f});
        m.Box(-3.0f, 3.0f, -9, -1, 8.6f, 10.2f, {0.85f, 0.3f, 0.2f}, false);
        emit("Boat Line", v);
        v.clear();
        tmodel::Hull(m, -40, 40, -8, 8, 0.0f, 6.0f, {0.2f, 0.25f, 0.32f}, true);
        for (int i = 0; i < 5; ++i) tmodel::ContainerStack(m, -32.0f + i * 12.5f, -6.5f, 2 + (i & 1), (uint32_t)(i * 7 + 3), false);
        m.Box(-38, -30, -6, 6, 6.0f, 14.0f, {0.92f, 0.92f, 0.9f});
        emit("Cargo Line", v);
        v.clear();
        tmodel::TrackX(m, 0, -24, 24);
        m.Box(-15, 15, -1.3f, 1.3f, 0.6f, 3.6f, {0.86f, 0.18f, 0.16f});
        m.Box(-15.1f, 15.1f, -1.35f, 1.35f, 2.0f, 3.0f, tmodel::kDark, false);
        emit("Tram Line", v);
    }
    js += "];\n";
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) { out = "cannot write " + path; return false; }
    fwrite(js.data(), 1, js.size(), f);
    fclose(f);
    out = "wrote " + path + " (" + std::to_string(js.size() / 1024) + " KB)";
    return true;
}
