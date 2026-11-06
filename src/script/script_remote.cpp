#include "script_remote.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "api_registry.h"
#include "luau_host.h"

namespace tbd::script {

namespace {

// A small JSON reader for argument arrays: null, true/false, numbers, strings, arrays. No objects (arguments are positional).
struct Json {
    const char* p;
    const char* end;
    std::string error;

    void Skip() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }

    bool Parse(Value& out, int depth = 0) {
        if (depth > 8) { error = "arguments nested too deeply"; return false; }
        Skip();
        if (p >= end) { error = "unexpected end of JSON"; return false; }
        if (*p == '[') {
            ++p;
            out = Value();
            out.type = VType::Array;
            Skip();
            if (p < end && *p == ']') { ++p; return true; }
            for (;;) {
                Value v;
                if (!Parse(v, depth + 1)) return false;
                out.a.push_back(std::move(v));
                Skip();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; return true; }
                error = "expected , or ] in JSON array";
                return false;
            }
        }
        if (*p == '"') {
            ++p;
            std::string s;
            while (p < end && *p != '"') {
                if (*p == '\\' && p + 1 < end) {
                    ++p;
                    switch (*p) {
                    case 'n': s += '\n'; break;
                    case 't': s += '\t'; break;
                    case 'r': s += '\r'; break;
                    case 'u': // four hex digits, Basic Multilingual Plane only
                        if (end - p >= 5) {
                            const unsigned cp = (unsigned)strtoul(std::string(p + 1, 4).c_str(), nullptr, 16);
                            if (cp < 0x80) s += (char)cp;
                            else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
                            else { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
                            p += 4;
                        }
                        break;
                    default: s += *p; break;
                    }
                } else s += *p;
                ++p;
            }
            if (p >= end) { error = "unterminated JSON string"; return false; }
            ++p;
            out = Value::Str(s);
            return true;
        }
        if (end - p >= 4 && !strncmp(p, "null", 4)) { p += 4; out = Value::Nil(); return true; }
        if (end - p >= 4 && !strncmp(p, "true", 4)) { p += 4; out = Value::Bool(true); return true; }
        if (end - p >= 5 && !strncmp(p, "false", 5)) { p += 5; out = Value::Bool(false); return true; }
        char* stop = nullptr;
        const double n = strtod(p, &stop);
        if (stop == p) { error = std::string("bad JSON near '") + std::string(p, std::min<size_t>(10, (size_t)(end - p))) + "'"; return false; }
        p = stop;
        out = Value::Num(n);
        return true;
    }
};

// A JSON array of numbers becomes a vec3 or quat when the parameter says so.
bool Coerce(Value& v, const std::string& declared, std::string& error) {
    std::string t = declared;
    if (!t.empty() && t.back() == '?') t.pop_back();
    if (v.type != VType::Array || (t != "vec3" && t != "quat")) return true;
    const size_t need = t == "vec3" ? 3 : 4;
    if (v.a.size() != need) { error = "expected " + std::to_string(need) + " numbers for " + t; return false; }
    Value r = t == "vec3" ? Value::Vec(0, 0, 0) : Value::Rot(0, 0, 0, 1);
    for (size_t i = 0; i < need; ++i) {
        if (v.a[i].type != VType::Number) { error = t + " needs numbers"; return false; }
        r.v[i] = (float)v.a[i].n;
    }
    v = r;
    return true;
}

void Escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c >= 0x20) out += (char)c;
    }
    out += '"';
}

void Num(std::string& out, double n) {
    char b[48];
    if (std::isfinite(n)) snprintf(b, sizeof(b), "%.9g", n);
    else snprintf(b, sizeof(b), "null");
    out += b;
}

void ToJson(std::string& out, const Value& v) {
    switch (v.type) {
    case VType::Nil: out += "null"; break;
    case VType::Bool: out += v.b ? "true" : "false"; break;
    case VType::Number: Num(out, v.n); break;
    case VType::String: Escape(out, v.s); break;
    case VType::Vec3: out += '['; for (int i = 0; i < 3; ++i) { if (i) out += ','; Num(out, v.v[i]); } out += ']'; break;
    case VType::Quat: out += '['; for (int i = 0; i < 4; ++i) { if (i) out += ','; Num(out, v.v[i]); } out += ']'; break;
    case VType::Array:
        out += '[';
        for (size_t i = 0; i < v.a.size(); ++i) { if (i) out += ','; ToJson(out, v.a[i]); }
        out += ']';
        break;
    }
}

} // namespace

bool RemoteScriptCommand(LuauHost& host, const std::vector<std::string>& args, std::string& out) {
    const std::string& cmd = args[0];
    if (cmd == "script.list") {
        out = Registry::Get().ListJson();
        return true;
    }
    if (cmd == "script.call") {
        if (args.size() < 3) { out = "script.call|module|function|[json args]"; return false; }
        const ApiFunction* f = Registry::Get().Find(args[1], args[2]);
        if (!f) { out = "no such function tbd." + args[1] + "." + args[2] + " (script.list shows what exists)"; return false; }
        std::string json; // the JSON may itself contain '|', which the pipe split apart
        for (size_t i = 3; i < args.size(); ++i) json += (i > 3 ? "|" : "") + args[i];
        std::vector<Value> values;
        if (!json.empty()) {
            Json j{json.data(), json.data() + json.size(), {}};
            Value arr;
            if (!j.Parse(arr) || arr.type != VType::Array) { out = j.error.empty() ? "arguments must be a JSON array" : j.error; return false; }
            values = std::move(arr.a);
        }
        for (size_t i = 0; i < values.size() && i < f->params.size(); ++i) {
            std::string why;
            if (!Coerce(values[i], f->params[i].type, why)) { out = "tbd." + args[1] + "." + args[2] + " argument " + std::to_string(i + 1) + ": " + why; return false; }
        }
        Value result;
        std::string error;
        if (!Registry::Get().Call(args[1], args[2], values, result, error)) { out = error; return false; }
        out.clear();
        ToJson(out, result);
        return true;
    }
    if (cmd == "script.eval") {
        std::string code;
        for (size_t i = 1; i < args.size(); ++i) code += (i > 1 ? "|" : "") + args[i];
        for (char& c : code)
            if (c == '\x1f') c = '\n';
        std::string result;
        const bool ok = host.Eval(code, "=script.eval", result);
        out = result;
        return ok;
    }
    out = "unknown script command " + cmd + " (script.list script.call script.eval)";
    return false;
}

} // namespace tbd::script
