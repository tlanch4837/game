#include "luau_host.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "lua.h"
#include "luacode.h"
#include "lualib.h"

#include "api_registry.h"

namespace tbd::script {

double HostClockSeconds() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

namespace {

// Allocator with a hard cap: going over it fails the allocation, which Luau reports as a script error, not a crash.
void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    LuauHost* host = (LuauHost*)ud;
    if (nsize == 0) {
        if (ptr) { free(ptr); host->m_bytes -= osize; }
        return nullptr;
    }
    const size_t before = ptr ? osize : 0;
    if (host->m_bytes - before + nsize > host->m_cap) return nullptr;
    void* p = realloc(ptr, nsize);
    if (p) host->m_bytes = host->m_bytes - before + nsize;
    return p;
}

LuauHost* HostOf(lua_State* L) { return (LuauHost*)lua_getthreaddata(lua_mainthread(L)); }

// Checked every function call and loop back-edge: a script past its deadline is aborted.
void Interrupt(lua_State* L, int) {
    LuauHost* host = HostOf(L);
    if (host && host->m_deadline > 0 && HostClockSeconds() > host->m_deadline) {
        host->m_deadline = 0; // raise once; the error unwinds to the pcall
        luaL_error(L, "script exceeded its time limit");
    }
}

Value ToValue(lua_State* L, int idx) {
    switch (lua_type(L, idx)) {
    case LUA_TBOOLEAN: return Value::Bool(lua_toboolean(L, idx) != 0);
    case LUA_TNUMBER: return Value::Num(lua_tonumber(L, idx));
    case LUA_TSTRING: return Value::Str(lua_tostring(L, idx));
    case LUA_TVECTOR: { const float* v = lua_tovector(L, idx); return Value::Vec(v[0], v[1], v[2]); }
    case LUA_TTABLE: {
        idx = lua_absindex(L, idx);
        lua_getfield(L, idx, "w");
        const bool quat = !lua_isnil(L, -1);
        lua_pop(L, 1);
        if (quat) {
            float q[4];
            const char* keys[4] = {"x", "y", "z", "w"};
            for (int i = 0; i < 4; ++i) { lua_getfield(L, idx, keys[i]); q[i] = (float)lua_tonumber(L, -1); lua_pop(L, 1); }
            return Value::Rot(q[0], q[1], q[2], q[3]);
        }
        Value arr;
        arr.type = VType::Array;
        const int n = lua_objlen(L, idx);
        for (int i = 1; i <= n; ++i) { lua_rawgeti(L, idx, i); arr.a.push_back(ToValue(L, -1)); lua_pop(L, 1); }
        return arr;
    }
    default: return Value::Nil();
    }
}

void Push(lua_State* L, const Value& v) {
    switch (v.type) {
    case VType::Nil: lua_pushnil(L); break;
    case VType::Bool: lua_pushboolean(L, v.b); break;
    case VType::Number: lua_pushnumber(L, v.n); break;
    case VType::String: lua_pushlstring(L, v.s.data(), v.s.size()); break;
    case VType::Vec3: lua_pushvector(L, v.v[0], v.v[1], v.v[2]); break;
    case VType::Quat:
        lua_createtable(L, 0, 4);
        lua_pushnumber(L, v.v[0]); lua_setfield(L, -2, "x");
        lua_pushnumber(L, v.v[1]); lua_setfield(L, -2, "y");
        lua_pushnumber(L, v.v[2]); lua_setfield(L, -2, "z");
        lua_pushnumber(L, v.v[3]); lua_setfield(L, -2, "w");
        break;
    case VType::Array:
        lua_createtable(L, (int)v.a.size(), 0);
        for (size_t i = 0; i < v.a.size(); ++i) { Push(L, v.a[i]); lua_rawseti(L, -2, (int)i + 1); }
        break;
    }
}

// One C function serves every registry entry; the entry is the closure's upvalue.
int CallRegistered(lua_State* L) {
    const ApiFunction* f = (const ApiFunction*)lua_tolightuserdata(L, lua_upvalueindex(1));
    std::vector<Value> args;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) args.push_back(ToValue(L, i));
    while (!args.empty() && args.back().type == VType::Nil) args.pop_back(); // trailing nils are "not given"
    std::string error;
    if (!Registry::CheckArgs(*f, args, error)) luaL_error(L, "%s", error.c_str());
    Value out;
    if (!f->fn(args, out, error)) luaL_error(L, "tbd.%s.%s failed: %s", f->module.c_str(), f->name.c_str(), error.c_str());
    if (f->returns.empty()) return 0;
    Push(L, out);
    return 1;
}

int LuaPrint(lua_State* L) {
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t len;
        const char* s = luaL_tolstring(L, i, &len);
        if (i > 1) line += '\t';
        line.append(s, len);
        lua_pop(L, 1);
    }
    if (LuauHost* h = HostOf(L)) h->Log(line.c_str());
    return 0;
}

} // namespace

bool LuauHost::Init(const std::string& scriptsDir, void (*log)(const char*)) {
    Shutdown();
    m_dir = scriptsDir;
    m_log = log;
    m_bytes = 0;
    m_L = lua_newstate(Alloc, this);
    if (!m_L) return false;
    lua_setthreaddata(m_L, this);
    luaL_openlibs(m_L);
    lua_callbacks(m_L)->interrupt = Interrupt;

    // tbd.<module>.<name> for every registry entry
    lua_newtable(m_L);
    for (const ApiFunction& f : Registry::Get().All()) {
        lua_getfield(m_L, -1, f.module.c_str());
        if (lua_isnil(m_L, -1)) {
            lua_pop(m_L, 1);
            lua_newtable(m_L);
            lua_pushvalue(m_L, -1);
            lua_setfield(m_L, -3, f.module.c_str());
        }
        lua_pushlightuserdata(m_L, (void*)&f);
        lua_pushcclosure(m_L, CallRegistered, (f.module + "." + f.name).c_str(), 1);
        lua_setfield(m_L, -2, f.name.c_str());
        lua_pop(m_L, 1);
    }
    // tbd.log.print goes to the engine log too; Lua's print is redirected
    lua_pushcfunction(m_L, LuaPrint, "print");
    lua_setglobal(m_L, "print");
    lua_setglobal(m_L, "tbd");

    // The runtime (instances, hooks, coroutines, events) is Luau source embedded in the binary.
    static const char kPrelude[] =
#include "luau_prelude.inl"
        ;
    std::string err;
    if (!Eval(kPrelude, "=prelude", err)) { Log(("script runtime failed to load: " + err).c_str()); Shutdown(); return false; }

    for (const char* name : {"os", "debug", "loadstring", "getfenv", "setfenv"}) { // no clock, no reflection, no compiling strings at runtime
        lua_pushnil(m_L);
        lua_setglobal(m_L, name);
    }
    luaL_sandbox(m_L); // libraries and globals become read-only; scripts get a safe environment
    lua_getglobal(m_L, "tbd");
    lua_setreadonly(m_L, -1, true);
    lua_pop(m_L, 1);
    return true;
}

void LuauHost::Shutdown() {
    if (m_L) { lua_close(m_L); m_L = nullptr; }
    m_bytes = 0;
    m_fixedAccum = 0;
    m_known.clear();
}

bool LuauHost::Protected(int nargs, int nresults, const char* what) {
    m_deadline = HostClockSeconds() + m_limitMs / 1000.0;
    const int status = lua_pcall(m_L, nargs, nresults, 0);
    m_deadline = 0;
    if (status == LUA_OK) return true;
    const char* msg = lua_tostring(m_L, -1);
    char line[600];
    snprintf(line, sizeof(line), "script error (%s): %s", what, msg ? msg : "?");
    Log(line);
    lua_pop(m_L, 1);
    return false;
}

bool LuauHost::Eval(const std::string& source, const std::string& chunkName, std::string& result) {
    if (!m_L) { result = "scripting is not running"; return false; }
    size_t size = 0;
    lua_CompileOptions opts = {};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    char* bytecode = luau_compile(source.c_str(), source.size(), &opts, &size);
    const int top = lua_gettop(m_L);
    const int loaded = luau_load(m_L, chunkName.c_str(), bytecode, size, 0);
    free(bytecode);
    if (loaded != 0) {
        result = lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "load failed";
        lua_settop(m_L, top);
        return false;
    }
    m_deadline = HostClockSeconds() + m_limitMs / 1000.0;
    const int status = lua_pcall(m_L, 0, 1, 0);
    m_deadline = 0;
    if (status != LUA_OK) {
        result = lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "error";
        lua_settop(m_L, top);
        return false;
    }
    size_t len = 0;
    const char* s = lua_isnil(m_L, -1) ? nullptr : luaL_tolstring(m_L, -1, &len);
    result = s ? std::string(s, len) : std::string();
    lua_settop(m_L, top);
    return true;
}

bool LuauHost::ReadScript(const std::string& relPath, std::string& source) {
    // Scripts come from the project's Scripts folder only: no absolute paths, no "..".
    if (relPath.empty() || relPath.find("..") != std::string::npos || relPath.find(':') != std::string::npos || relPath[0] == '/' || relPath[0] == '\\') {
        Log(("script path rejected: " + relPath).c_str());
        return false;
    }
    std::ifstream in(m_dir + "/" + relPath, std::ios::binary);
    if (!in) return false;
    std::stringstream ss;
    ss << in.rdbuf();
    source = ss.str();
    return true;
}

static bool FileStampOf(const std::string& path, int64_t& out) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(std::filesystem::u8path(path), ec);
    if (ec) return false;
    out = (int64_t)t.time_since_epoch().count();
    return true;
}

bool LuauHost::LoadDef(const std::string& relPath) {
    if (!m_L) return false;
    std::string source;
    if (!ReadScript(relPath, source)) { Log(("script not found: " + relPath).c_str()); return false; }
    Stamp& st = m_known[relPath]; // recorded even on failure, so a broken file is retried only when it changes again
    FileStampOf(m_dir + "/" + relPath, st.time);
    st.hash = std::hash<std::string>()(source);

    size_t size = 0;
    lua_CompileOptions opts = {};
    opts.optimizationLevel = 1;
    opts.debugLevel = 1;
    char* bytecode = luau_compile(source.c_str(), source.size(), &opts, &size);
    const int top = lua_gettop(m_L);
    // Stack: tbd.__define, path, then the chunk. Running the chunk leaves its returned table where the chunk was.
    lua_getglobal(m_L, "tbd");
    lua_getfield(m_L, -1, "__define");
    lua_remove(m_L, -2);
    lua_pushstring(m_L, relPath.c_str());
    const std::string chunk = "=" + relPath;
    const int loaded = luau_load(m_L, chunk.c_str(), bytecode, size, 0);
    free(bytecode);
    if (loaded != 0) {
        Log(("script error (" + relPath + "): " + (lua_tostring(m_L, -1) ? lua_tostring(m_L, -1) : "load failed")).c_str());
        lua_settop(m_L, top);
        return false;
    }
    if (!Protected(0, 1, relPath.c_str())) { lua_settop(m_L, top); return false; } // __define, path, def
    const bool ok = Protected(2, 1, relPath.c_str());                                // __define(path, def)
    const bool defined = ok && lua_toboolean(m_L, -1);
    lua_settop(m_L, top);
    if (defined) ++m_generation;
    return defined;
}

std::vector<LuauHost::PropInfo> LuauHost::DeclaredProps(const std::string& relPath) {
    std::vector<PropInfo> props;
    if (!m_L) return props;
    if (m_known.find(relPath) == m_known.end() && !LoadDef(relPath)) return props;
    const int top = lua_gettop(m_L);
    lua_getglobal(m_L, "tbd");
    lua_getfield(m_L, -1, "__props");
    lua_remove(m_L, -2);
    lua_pushstring(m_L, relPath.c_str());
    if (Protected(1, 1, "__props") && lua_istable(m_L, -1)) {
        const int n = lua_objlen(m_L, -1);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(m_L, -1, i);
            PropInfo p;
            auto text = [&](const char* key) {
                lua_getfield(m_L, -1, key);
                std::string s = lua_isstring(m_L, -1) ? lua_tostring(m_L, -1) : "";
                lua_pop(m_L, 1);
                return s;
            };
            p.name = text("name");
            p.type = text("type");
            p.defaultText = text("default");
            lua_getfield(m_L, -1, "min");
            if (lua_isnumber(m_L, -1)) { p.hasMin = true; p.min = lua_tonumber(m_L, -1); }
            lua_pop(m_L, 1);
            lua_getfield(m_L, -1, "max");
            if (lua_isnumber(m_L, -1)) { p.hasMax = true; p.max = lua_tonumber(m_L, -1); }
            lua_pop(m_L, 1);
            lua_pop(m_L, 1);
            props.push_back(p);
        }
    }
    lua_settop(m_L, top);
    return props;
}

bool LuauHost::ScriptExists(const std::string& relPath) const {
    if (relPath.empty() || relPath.find("..") != std::string::npos || relPath.find(':') != std::string::npos || relPath[0] == '/' || relPath[0] == '\\') return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::u8path(m_dir + "/" + relPath), ec);
}

std::vector<std::string> LuauHost::ListScripts() const {
    std::vector<std::string> out;
    std::error_code ec;
    const std::filesystem::path root = std::filesystem::u8path(m_dir);
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".luau") continue;
        const auto u8 = std::filesystem::relative(it->path(), root, ec).generic_u8string();
        std::string rel(u8.begin(), u8.end());
        const bool typeStubs = rel.size() > 7 && rel.compare(rel.size() - 7, 7, ".d.luau") == 0; // tbd_api.d.luau is editor type info, not a script
        if (rel == "manifest.luau" || rel.rfind("tests/", 0) == 0 || typeStubs) continue;
        out.push_back(rel);
    }
    std::sort(out.begin(), out.end());
    return out;
}

bool LuauHost::LoadScript(const std::string& relPath) {
    if (!LoadDef(relPath)) return false;
    lua_getglobal(m_L, "tbd");
    lua_getfield(m_L, -1, "__instance");
    lua_remove(m_L, -2);
    lua_pushstring(m_L, relPath.c_str());
    return Protected(1, 0, relPath.c_str());
}

bool LuauHost::AddEntityScript(int entity, const std::string& relPath, const std::string& props) {
    if (!m_L) return false;
    if (m_known.find(relPath) == m_known.end() && !LoadDef(relPath)) return false;
    lua_getglobal(m_L, "tbd");
    lua_getfield(m_L, -1, "__instance");
    lua_remove(m_L, -2);
    lua_pushstring(m_L, relPath.c_str());
    lua_pushinteger(m_L, entity);
    lua_newtable(m_L);
    size_t at = 0;
    while (at < props.size()) { // "key=value;key=value"
        size_t end = props.find(';', at);
        if (end == std::string::npos) end = props.size();
        const size_t eq = props.find('=', at);
        if (eq != std::string::npos && eq < end) {
            lua_pushlstring(m_L, props.data() + eq + 1, end - eq - 1);
            lua_setfield(m_L, -2, props.substr(at, eq - at).c_str());
        }
        at = end + 1;
    }
    return Protected(3, 0, relPath.c_str());
}

int LuauHost::PollReload(bool force) {
    if (!m_L) return 0;
    const double now = HostClockSeconds();
    if (!force && now - m_lastPoll < 0.25) return 0;
    m_lastPoll = now;
    std::vector<std::string> paths;
    for (const auto& k : m_known) paths.push_back(k.first);
    int reloaded = 0;
    for (const std::string& path : paths) {
        int64_t stamp = 0;
        if (!FileStampOf(m_dir + "/" + path, stamp)) continue; // deleted or renamed: the loaded code stays
        const Stamp old = m_known[path];
        if (!force && stamp == old.time) continue;
        std::string source;
        if (!ReadScript(path, source)) continue;
        if (std::hash<std::string>()(source) == old.hash) { m_known[path].time = stamp; continue; }
        Log(("script changed: " + path).c_str());
        if (LoadDef(path)) { ++reloaded; Log(("script reloaded: " + path).c_str()); }
        else Log(("script not reloaded, the running code stays: " + path).c_str());
    }
    return reloaded;
}

bool LuauHost::LoadManifest() {
    std::string source;
    if (!ReadScript("manifest.luau", source)) return true;
    std::string result;
    if (!Eval(source, "=manifest.luau", result)) { Log(("manifest error: " + result).c_str()); return false; }
    // The manifest returns a table of names; re-run it to read them (Eval flattens to a string).
    lua_State* L = m_L;
    size_t size = 0;
    lua_CompileOptions opts = {};
    char* bytecode = luau_compile(source.c_str(), source.size(), &opts, &size);
    const int top = lua_gettop(L);
    if (luau_load(L, "=manifest.luau", bytecode, size, 0) != 0 || lua_pcall(L, 0, 1, 0) != LUA_OK || !lua_istable(L, -1)) {
        free(bytecode);
        lua_settop(L, top);
        Log("manifest.luau must return a list of script paths");
        return false;
    }
    free(bytecode);
    std::vector<std::string> names;
    const int n = lua_objlen(L, -1);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (const char* s = lua_tostring(L, -1)) names.push_back(s);
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    bool all = true;
    for (const std::string& name : names) all = LoadScript(name) && all;
    return all;
}

bool LuauHost::CallHook(const char* name, bool hasArg, double arg) {
    if (!m_L) return false;
    lua_getglobal(m_L, "tbd");
    lua_getfield(m_L, -1, "__hook");
    lua_remove(m_L, -2);
    lua_pushstring(m_L, name);
    int n = 1;
    if (hasArg) { lua_pushnumber(m_L, arg); n = 2; }
    return Protected(n, 0, name);
}

namespace {
bool CallTbd(LuauHost& host, const char* fn, bool hasArg, double arg) {
    lua_State* L = host.State();
    if (!L) return false;
    lua_getglobal(L, "tbd");
    lua_getfield(L, -1, fn);
    lua_remove(L, -2);
    int n = 0;
    if (hasArg) { lua_pushnumber(L, arg); n = 1; }
    host.m_deadline = HostClockSeconds() + 0.25;
    const int st = lua_pcall(L, n, 0, 0);
    host.m_deadline = 0;
    if (st == LUA_OK) return true;
    char line[600];
    snprintf(line, sizeof(line), "script error (%s): %s", fn, lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
    host.Log(line);
    lua_pop(L, 1);
    return false;
}
} // namespace

void LuauHost::Start() {
    m_fixedAccum = 0;
    CallHook("on_start", false, 0);
}

void LuauHost::Update(float dt) {
    if (!m_L) return;
    CallTbd(*this, "__frame", true, dt);
    m_fixedAccum += dt;
    int steps = 0;
    while (m_fixedAccum >= 1.0f / 60.0f && steps < 8) { // capped so a long hitch does not spiral
        CallHook("on_fixed_update", true, 1.0 / 60.0);
        m_fixedAccum -= 1.0f / 60.0f;
        ++steps;
    }
    if (steps == 8) m_fixedAccum = 0;
    CallHook("on_update", true, dt);
    CallTbd(*this, "__routines", true, dt);
    CallHook("on_late_update", true, dt);
}

void LuauHost::Stop() {
    CallTbd(*this, "__stop", false, 0);
}

} // namespace tbd::script
