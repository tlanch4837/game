// Prints every buildable item the Glass Dock GUI lists, as JSON, for the preview renderer (tools/gui_atlas/render.html).
//   cl /nologo /std:c++20 /EHsc /I src tools\gui_atlas\dump_catalog.cpp /Fe:obj\dump_catalog.exe && obj\dump_catalog.exe > tools\gui_atlas\catalog.json
#include <cstdio>
#include <string>

#include "city/industry.h"
#include "city/road_tool.h"
#include "city/transport.h"
#include "city/utilities.h"
#include "city/zoning.h"

using namespace tbd::city;

static std::string Esc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '"' || c == '\\') o += '\\';
        o += c;
    }
    return o;
}

int main() {
    bool first = true;
    auto item = [&](const char* kind, const std::string& name, const std::string& extra) {
        std::printf("%s\n {\"kind\":\"%s\",\"name\":\"%s\"%s}", first ? "[" : ",", kind, Esc(name).c_str(), extra.c_str());
        first = false;
    };
    const auto& P = Profiles();
    for (const Asset& a : Assets()) {
        char e[256];
        if (a.kind == AssetKind::Road) std::snprintf(e, sizeof(e), ",\"tab\":%d,\"profile\":\"%s\"", (int)a.tab, Esc(P[a.profile].name).c_str());
        else {
            std::string arms;
            for (float d : a.armDeg) arms += (arms.empty() ? "" : ",") + std::to_string(d);
            std::snprintf(e, sizeof(e), ",\"tab\":%d,\"profile\":\"%s\",\"arms\":[%s],\"radius\":%g", (int)a.tab, Esc(P[a.profile].name).c_str(), arms.c_str(), a.radius);
        }
        item(a.kind == AssetKind::Road ? "road" : a.kind == AssetKind::Intersection ? "intersection" : "roundabout", a.name, e);
    }
    for (const BuildingDef& d : Defs()) {
        char e[512];
        std::snprintf(e, sizeof(e), ",\"tab\":%d,\"model\":%d,\"w\":%d,\"d\":%d,\"height\":%g,\"glb\":\"%s\"", (int)d.tab, (int)d.model, d.w, d.d, d.height, d.glb ? d.glb : "");
        item("transport", d.name, e);
    }
    for (const UtilDef& d : UtilDefs()) {
        char e[512];
        std::snprintf(e, sizeof(e), ",\"tab\":%d,\"glb\":\"%s\"", (int)d.tab, d.file ? d.file : "");
        item("utility", d.name, e);
    }
    for (const ZoneType& z : ZoneTypes()) {
        char e[256];
        std::snprintf(e, sizeof(e), ",\"cat\":%d,\"dir\":\"%s\",\"prefix\":\"%s\"", (int)z.cat, z.dir, z.prefix);
        item("zone", z.name, e);
    }
    for (int k = 0; k < (int)IndKind::Count; ++k) item("industry", IndTypeOf((IndKind)k).name, "");
    std::printf("\n]\n");
}
