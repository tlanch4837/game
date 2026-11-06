// Character preset files (gameenginetools\face_maker\character_preset.schema.json, schema_version 1): what the creator panel
// saves and loads. The face block is the identity sliders (morphs.json names, -1..1, 0 = Wyatt, age already included); every
// other block (skin, hair, wrinkles, mouth, eyes) is kept as read and written back unchanged, since the engine does not edit
// those yet. Output follows face_maker\index.html's Save preset: rounded to 3 decimals, zero sliders left out.
#pragma once
#include <cstdio>
#include <string>
#include <vector>

#include "face_morphs.h"

namespace tbd::character {

struct CharacterPreset {
    std::string name = "custom";
    std::string restChecksum;  // as read (may be empty)
    long long seed = -1;       // -1 = not from Randomize
    float age = 0.0f;
    std::vector<float> face;   // per MorphSet slider
    Json extra;                // the other top-level blocks, as read

    // Reads `path` against `set`. `warning` gets a note for a checksum mismatch or unknown slider names (still loads).
    bool Load(const std::wstring& path, const MorphSet& set, std::string* err, std::string* warning) {
        std::vector<uint8_t> bytes;
        if (!ReadBytes(path, bytes)) { if (err) *err = "cannot read the preset"; return false; }
        Json doc;
        if (!JsonParser((const char*)bytes.data(), bytes.size()).Parse(doc) || doc.type != Json::Object) { if (err) *err = "the preset is not JSON"; return false; }
        if (!doc.Find("schema_version") || doc.Find("schema_version")->Int() != 1) { if (err) *err = "schema_version must be 1"; return false; }
        face.assign(set.sliders.size(), 0.0f);
        extra = Json();
        extra.type = Json::Object;
        std::string warn;
        for (const auto& [k, v] : doc.o) {
            if (k == "name" && v.type == Json::String) name = v.s;
            else if (k == "rest_checksum") restChecksum = v.s;
            else if (k == "seed" && v.type == Json::Number) seed = (long long)v.n;
            else if (k == "age" && v.type == Json::Number) age = (float)v.n;
            else if (k == "face") {
                for (const auto& [fk, fv] : v.o) {
                    const int i = set.Find(fk);
                    if (i < 0) { warn += (warn.empty() ? "unknown sliders: " : ", ") + fk; continue; }
                    face[(size_t)i] = std::clamp((float)fv.Num(), -1.0f, 1.0f);
                }
            } else if (k != "schema_version" && k != "base_mesh") extra.o.push_back({k, v});
        }
        if (!restChecksum.empty() && !set.restChecksum.empty() && restChecksum != set.restChecksum)
            warn = "preset was made for another base mesh (checksum " + restChecksum + ")" + (warn.empty() ? "" : "; " + warn);
        if (warning) *warning = warn;
        return true;
    }

    bool Save(const std::wstring& path, const MorphSet& set) const {
        std::string s = "{\n \"schema_version\": 1,\n \"name\": " + Quote(name) + ",\n \"base_mesh\": " + Quote(set.mesh.empty() ? "body_head.glb" : set.mesh) +
                        ",\n \"rest_checksum\": " + Quote(set.restChecksum) + ",\n";
        if (seed >= 0) s += " \"seed\": " + std::to_string(seed) + ",\n";
        s += " \"age\": " + Num(age) + ",\n \"face\": {";
        bool first = true;
        for (size_t i = 0; i < set.sliders.size() && i < face.size(); ++i) {
            const double v = std::round(face[i] * 1000.0) / 1000.0;
            if (v == 0.0) continue;
            s += std::string(first ? "\n" : ",\n") + "  " + Quote(set.sliders[i].name) + ": " + Num(v);
            first = false;
        }
        s += first ? "}" : "\n }";
        bool wrinkles = false;
        for (const auto& [k, v] : extra.o) {
            wrinkles |= k == "wrinkles";
            s += ",\n " + Quote(k) + ": " + Write(v, 1);
        }
        if (!wrinkles) s += ",\n \"wrinkles\": {\"age\": " + Num(age) + "}";
        s += "\n}\n";
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        const bool ok = fwrite(s.data(), 1, s.size(), f) == s.size();
        fclose(f);
        return ok;
    }

    static std::string Quote(const std::string& v) {
        std::string o = "\"";
        for (const char c : v) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else if ((unsigned char)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; }
            else o += c;
        }
        return o + "\"";
    }
    static std::string Num(double v) {
        char b[32];
        snprintf(b, sizeof(b), "%.10g", v);
        return b;
    }
    static std::string Write(const Json& j, int depth) {
        switch (j.type) {
        case Json::Null: return "null";
        case Json::Bool: return j.b ? "true" : "false";
        case Json::Number: return Num(j.n);
        case Json::String: return Quote(j.s);
        case Json::Array: {
            std::string o = "[";
            for (size_t i = 0; i < j.a.size(); ++i) o += (i ? ", " : "") + Write(j.a[i], depth + 1);
            return o + "]";
        }
        case Json::Object: {
            std::string o = "{";
            for (size_t i = 0; i < j.o.size(); ++i) o += std::string(i ? ", " : "") + Quote(j.o[i].first) + ": " + Write(j.o[i].second, depth + 1);
            return o + "}";
        }
        }
        return "null";
    }
};

} // namespace tbd::character
