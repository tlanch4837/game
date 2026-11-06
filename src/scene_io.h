// Scene files (.tbdscene): plain text, one record per line, easy to read and diff.
//   TBDSCENE 1
//   T|enabled|seed|amplitude|radius
//   E|name|Mesh|px py pz|qx qy qz qw|sx sy sz|r g b[|meshPath]   (meshPath only for Mesh = Imported)
//   F|entity index|clipPath   (a face mocap clip drives that entity's head rotation; after all E lines)
//   I|name|meshPath|file.bin[|maxDistance[|shadowDistance]]   (one asset drawn many times; file relative to the assets folder)
//   A|entity index|clipPath   (a glTF animation poses that entity's skin; after all E lines)
//   S|entity index|script path|key=value;key=value   (a Luau script on the entity, props optional; after all E lines; older readers skip it)
//   N|entity index|routine|wanderRadius|reactRadius|indoors x y z or "-"|patrol x y z;x y z...   (the entity is an AI agent in Play; after all E lines)
#pragma once
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#include "project.h"
#include "scene.h"
#include "terrain.h"

namespace tbd {

inline const wchar_t* kSceneExtension = L".tbdscene";

inline std::string SerializeScene(const Scene& scene, const TerrainParams& terrain) {
    std::ostringstream out;
    out << "TBDSCENE 1\n";
    out << "T|" << (terrain.enabled ? 1 : 0) << '|' << terrain.seed << '|' << terrain.amplitude << '|' << terrain.radius
        << '\n';
    if (scene.ambientFill != 1.0f || scene.exposure > 0.0f || scene.hour >= 0.0f)
        out << "L|" << scene.ambientFill << '|' << scene.exposure << '|' << scene.hour << '\n';
    for (const Entity& e : scene.entities) {
        std::string name = e.name;
        for (char& c : name)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        const Quat& rot = e.faceClip.empty() ? e.rotation : e.faceBase; // a driven entity's rotation is animated
        out << "E|" << name << '|' << MeshKindName(e.mesh) << '|' << e.position.x << ' ' << e.position.y << ' '
            << e.position.z << '|' << rot.x << ' ' << rot.y << ' ' << rot.z << ' ' << rot.w
            << '|' << e.scale.x << ' ' << e.scale.y << ' ' << e.scale.z << '|' << e.color.x << ' ' << e.color.y << ' '
            << e.color.z;
        if (e.mesh == MeshKind::Imported) {
            std::string path = e.meshPath;
            for (char& c : path)
                if (c == '|' || c == '\n' || c == '\r') c = ' ';
            out << '|' << path;
        }
        out << '\n';
    }
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        std::string path = scene.entities[i].faceClip;
        if (path.empty()) continue;
        for (char& c : path)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        out << "F|" << i << '|' << path << '\n';
    }
    for (const InstanceSet& set : scene.instances)
        out << "I|" << set.name << '|' << set.meshPath << '|' << set.file << '|' << set.maxDistance << '|' << set.shadowDistance << '\n';
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        std::string path = scene.entities[i].animClip;
        if (path.empty()) continue;
        for (char& c : path)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        out << "A|" << i << '|' << path << '\n';
    }
    for (size_t i = 0; i < scene.entities.size(); ++i) {
        const NpcInfo& n = scene.entities[i].npc;
        if (!n.enabled) continue;
        std::string routine = n.routine;
        for (char& c : routine)
            if (c == '|' || c == '\n' || c == '\r') c = ' ';
        out << "N|" << i << '|' << routine << '|' << n.wanderRadius << '|' << n.reactRadius << '|';
        if (n.hasIndoors) out << n.indoors.x << ' ' << n.indoors.y << ' ' << n.indoors.z;
        else out << '-';
        out << '|';
        for (size_t k = 0; k < n.patrol.size(); ++k)
            out << (k ? ";" : "") << n.patrol[k].x << ' ' << n.patrol[k].y << ' ' << n.patrol[k].z;
        out << '\n';
    }
    for (size_t i = 0; i < scene.entities.size(); ++i)
        for (const EntityScript& sc : scene.entities[i].scripts) {
            std::string path = sc.path, props = sc.props;
            for (char& c : path)
                if (c == '|' || c == '\n' || c == '\r') c = ' ';
            for (char& c : props)
                if (c == '|' || c == '\n' || c == '\r') c = ' ';
            out << "S|" << i << '|' << path << '|' << props << '\n';
        }
    return out.str();
}

namespace scene_io_detail {

inline std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(sep, start);
        parts.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    return parts;
}

// Reads exactly `count` floats separated by spaces.
inline bool Floats(const std::string& s, float* out, int count) {
    std::istringstream in(s);
    for (int i = 0; i < count; ++i)
        if (!(in >> out[i])) return false;
    return true;
}

} // namespace scene_io_detail

// Replaces `scene` (and `terrain`) with the file's contents. Leaves both untouched and returns false
// on a file that isn't a scene.
inline bool ParseScene(const std::string& text, Scene& scene, TerrainParams& terrain) {
    using namespace scene_io_detail;
    std::istringstream in(text);
    std::string line;
    if (!std::getline(in, line) || line.rfind("TBDSCENE", 0) != 0) return false;

    Scene loaded;
    loaded.entities.clear();
    loaded.selected = -1;
    TerrainParams t = terrain;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::vector<std::string> f = Split(line, '|');
        if (f.size() >= 5 && f[0] == "T") {
            t.enabled = f[1] != "0";
            t.seed = std::atoi(f[2].c_str());
            t.amplitude = (float)std::atof(f[3].c_str());
            t.radius = std::clamp(std::atoi(f[4].c_str()), 2, 16);
        } else if (f.size() >= 2 && f[0] == "L") { // L|ambient fill|fixed exposure (0 = auto)
            loaded.ambientFill = std::clamp((float)std::atof(f[1].c_str()), 0.0f, 4.0f);
            if (f.size() >= 3) loaded.exposure = std::clamp((float)std::atof(f[2].c_str()), 0.0f, 50.0f);
            if (f.size() >= 4) loaded.hour = std::min((float)std::atof(f[3].c_str()), 24.0f);
        } else if (f.size() >= 7 && f[0] == "E") {
            Entity e;
            e.name = f[1];
            e.mesh = MeshKind::Cube;
            for (MeshKind kind : {MeshKind::Plane, MeshKind::Sphere, MeshKind::Tree, MeshKind::Rock})
                if (f[2] == MeshKindName(kind)) e.mesh = kind;
            if (f[2] == MeshKindName(MeshKind::Imported) && f.size() >= 8 && !f[7].empty()) {
                e.mesh = MeshKind::Imported;
                e.meshPath = f[7];
            }
            float p[3], q[4], s[3], c[3];
            if (!Floats(f[3], p, 3) || !Floats(f[4], q, 4) || !Floats(f[5], s, 3) || !Floats(f[6], c, 3)) continue;
            e.position = {p[0], p[1], p[2]};
            e.rotation = Normalize(Quat{q[0], q[1], q[2], q[3]});
            e.faceBase = e.rotation;
            e.scale = {s[0], s[1], s[2]};
            e.color = {c[0], c[1], c[2]};
            loaded.entities.push_back(e);
        } else if (f.size() >= 3 && f[0] == "F") { // F|entity index|clip path
            const int index = std::atoi(f[1].c_str());
            if (index >= 0 && index < (int)loaded.entities.size()) loaded.entities[index].faceClip = f[2];
        } else if (f.size() >= 4 && f[0] == "I") {
            InstanceSet set;
            set.name = f[1];
            set.meshPath = f[2];
            set.file = f[3];
            if (f.size() >= 5) set.maxDistance = (float)std::atof(f[4].c_str());
            if (f.size() >= 6) set.shadowDistance = (float)std::atof(f[5].c_str());
            loaded.instances.push_back(set);
        } else if (f.size() >= 3 && f[0] == "A") { // A|entity index|clip path
            const int index = std::atoi(f[1].c_str());
            if (index >= 0 && index < (int)loaded.entities.size()) loaded.entities[index].animClip = f[2];
        } else if (f.size() >= 3 && f[0] == "S") { // S|entity index|script path|props
            const int index = std::atoi(f[1].c_str());
            if (index >= 0 && index < (int)loaded.entities.size() && !f[2].empty())
                loaded.entities[index].scripts.push_back({f[2], f.size() >= 4 ? f[3] : std::string()});
        } else if (f.size() >= 7 && f[0] == "N") { // N|entity index|routine|wander|react|indoors|patrol
            const int index = std::atoi(f[1].c_str());
            if (index < 0 || index >= (int)loaded.entities.size()) continue;
            NpcInfo& n = loaded.entities[index].npc;
            n.enabled = true;
            n.routine = f[2];
            n.wanderRadius = (float)std::atof(f[3].c_str());
            n.reactRadius = (float)std::atof(f[4].c_str());
            float p[3];
            n.hasIndoors = f[5] != "-" && Floats(f[5], p, 3);
            if (n.hasIndoors) n.indoors = {p[0], p[1], p[2]};
            for (const std::string& point : Split(f[6], ';'))
                if (Floats(point, p, 3)) n.patrol.push_back({p[0], p[1], p[2]});
        }
    }
    loaded.selected = loaded.entities.empty() ? -1 : 0;
    scene = loaded;
    terrain = t;
    return true;
}

inline bool SaveSceneFile(const std::wstring& path, const Scene& scene, const TerrainParams& terrain) {
    return WriteAllText(path, SerializeScene(scene, terrain));
}

inline bool LoadSceneFile(const std::wstring& path, Scene& scene, TerrainParams& terrain) {
    std::string text;
    return ReadAllText(path, text) && ParseScene(text, scene, terrain);
}

} // namespace tbd
