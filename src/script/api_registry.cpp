#include "api_registry.h"

#include <cstdio>

namespace tbd::script {

EngineServices& Services() {
    static EngineServices s;
    return s;
}

Registry& Registry::Get() {
    static Registry r;
    return r;
}

void Registry::Add(ApiFunction f) {
    for (ApiFunction& e : m_functions)
        if (e.module == f.module && e.name == f.name) { e = std::move(f); return; } // re-registration replaces
    m_functions.push_back(std::move(f));
}

const ApiFunction* Registry::Find(const std::string& module, const std::string& name) const {
    for (const ApiFunction& f : m_functions)
        if (f.module == module && f.name == name) return &f;
    return nullptr;
}

static bool TypeMatches(const std::string& declared, const Value& v) {
    std::string t = declared;
    bool optional = false;
    if (!t.empty() && t.back() == '?') { optional = true; t.pop_back(); }
    if (v.type == VType::Nil) return optional || t == "any" || t == "entity"; // an entity argument may be nil (none)
    if (t == "any") return true;
    if (t == "bool") return v.type == VType::Bool;
    if (t == "number" || t == "entity") return v.type == VType::Number;
    if (t == "string") return v.type == VType::String;
    if (t == "vec3") return v.type == VType::Vec3;
    if (t == "quat") return v.type == VType::Quat;
    if (t.size() > 2 && t.compare(t.size() - 2, 2, "[]") == 0) return v.type == VType::Array;
    return false;
}

static const char* TypeName(VType t) {
    switch (t) {
    case VType::Nil: return "nil";
    case VType::Bool: return "bool";
    case VType::Number: return "number";
    case VType::String: return "string";
    case VType::Vec3: return "vec3";
    case VType::Quat: return "quat";
    case VType::Array: return "array";
    }
    return "?";
}

bool Registry::CheckArgs(const ApiFunction& f, const std::vector<Value>& args, std::string& error) {
    char buf[320];
    if (args.size() > f.params.size()) {
        snprintf(buf, sizeof(buf), "tbd.%s.%s takes %d argument(s), got %d", f.module.c_str(), f.name.c_str(), (int)f.params.size(), (int)args.size());
        error = buf;
        return false;
    }
    for (size_t i = 0; i < f.params.size(); ++i) {
        const Value none;
        const Value& v = i < args.size() ? args[i] : none;
        if (TypeMatches(f.params[i].type, v)) continue;
        snprintf(buf, sizeof(buf), "tbd.%s.%s: argument %d '%s' must be %s, got %s", f.module.c_str(), f.name.c_str(), (int)i + 1,
                 f.params[i].name.c_str(), f.params[i].type.c_str(), TypeName(v.type));
        error = buf;
        return false;
    }
    return true;
}

bool Registry::Call(const std::string& module, const std::string& name, const std::vector<Value>& args, Value& out, std::string& error) const {
    const ApiFunction* f = Find(module, name);
    if (!f) { error = "no such function tbd." + module + "." + name; return false; }
    if (!CheckArgs(*f, args, error)) return false;
    out = Value();
    return f->fn(args, out, error);
}

std::vector<std::string> Registry::Undocumented() const {
    std::vector<std::string> bad;
    for (const ApiFunction& f : m_functions)
        if (f.doc.empty()) bad.push_back(f.module + "." + f.name);
    return bad;
}

static std::string Signature(const ApiFunction& f) {
    std::string s = "tbd." + f.module + "." + f.name + "(";
    for (size_t i = 0; i < f.params.size(); ++i) s += (i ? ", " : "") + f.params[i].name + ": " + f.params[i].type;
    s += ")";
    if (!f.returns.empty()) s += " -> " + f.returns;
    return s;
}

std::string Registry::ReferenceMarkdown() const {
    std::string md = "# Scripting reference\n\nGenerated from the Engine API Registry (`tbd_script_test.exe --dump-docs`). Do not edit by hand.\n";
    std::string module;
    for (const ApiFunction& f : m_functions) {
        if (f.module != module) { module = f.module; md += "\n## tbd." + module + "\n\n"; }
        md += "- `" + Signature(f) + "`  (since " + f.since + ")";
        if (f.avail == Avail::PlayOnly) md += " [play only]";
        if (f.avail == Avail::EditorOnly) md += " [editor only]";
        md += "\n  " + f.doc + "\n";
    }
    return md;
}

static std::string LuauType(const std::string& t) {
    std::string base = t;
    bool optional = false;
    if (!base.empty() && base.back() == '?') { optional = true; base.pop_back(); }
    std::string out;
    if (base == "number" || base == "entity") out = base == "entity" ? "number?" : "number";
    else if (base == "vec3") out = "Vector3";
    else if (base == "quat") out = "{x: number, y: number, z: number, w: number}";
    else if (base.size() > 2 && base.compare(base.size() - 2, 2, "[]") == 0) out = "{" + LuauType(base.substr(0, base.size() - 2)) + "}";
    else out = base;
    if (optional && out.back() != '?') out += "?";
    return out;
}

std::string Registry::LuauTypeDefs() const {
    std::string d = "-- Generated from the Engine API Registry. Do not edit by hand.\ndeclare tbd: {\n";
    std::string module;
    for (const ApiFunction& f : m_functions) {
        if (f.module != module) {
            if (!module.empty()) d += "    },\n";
            module = f.module;
            d += "    " + module + ": {\n";
        }
        d += "        " + f.name + ": (";
        for (size_t i = 0; i < f.params.size(); ++i) d += (i ? ", " : "") + f.params[i].name + ": " + LuauType(f.params[i].type);
        d += ") -> " + (f.returns.empty() ? std::string("()") : LuauType(f.returns)) + ",\n";
    }
    if (!module.empty()) d += "    },\n";
    return d + "}\n";
}

static void JsonEscape(std::string& out, const std::string& s) {
    out += '"';
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if ((unsigned char)c >= 0x20) out += c;
    }
    out += '"';
}

std::string Registry::ListJson() const {
    std::string j = "[";
    for (size_t i = 0; i < m_functions.size(); ++i) {
        const ApiFunction& f = m_functions[i];
        if (i) j += ",";
        j += "{\"module\":";  JsonEscape(j, f.module);
        j += ",\"name\":";    JsonEscape(j, f.name);
        j += ",\"doc\":";     JsonEscape(j, f.doc);
        j += ",\"since\":";   JsonEscape(j, f.since);
        j += ",\"returns\":"; JsonEscape(j, f.returns);
        j += ",\"params\":[";
        for (size_t p = 0; p < f.params.size(); ++p) {
            if (p) j += ",";
            j += "{\"name\":"; JsonEscape(j, f.params[p].name);
            j += ",\"type\":"; JsonEscape(j, f.params[p].type);
            j += "}";
        }
        j += "]}";
    }
    return j + "]";
}

} // namespace tbd::script
