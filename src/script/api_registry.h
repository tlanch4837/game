// Engine API Registry: the one list of everything gameplay code may call (docs/scripting_spec.md section 4).
// A function is described once here; the Luau binding, the name-based C call (script_c_api.h), the pipe command,
// the generated docs/type stubs and the smoke test are all derived from this table. No Luau types in this header.
#pragma once
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace tbd::script {

enum class VType { Nil, Bool, Number, String, Vec3, Quat, Array };

// A value that crosses the script boundary. No pointers: entities are indices, assets are strings.
struct Value {
    VType type = VType::Nil;
    bool b = false;
    double n = 0;
    float v[4] = {0, 0, 0, 0}; // Vec3 uses 0..2, Quat uses x y z w
    std::string s;
    std::vector<Value> a;

    static Value Nil() { return {}; }
    static Value Bool(bool x) { Value r; r.type = VType::Bool; r.b = x; return r; }
    static Value Num(double x) { Value r; r.type = VType::Number; r.n = x; return r; }
    static Value Str(std::string x) { Value r; r.type = VType::String; r.s = std::move(x); return r; }
    static Value Vec(float x, float y, float z) { Value r; r.type = VType::Vec3; r.v[0] = x; r.v[1] = y; r.v[2] = z; return r; }
    static Value Rot(float x, float y, float z, float w) { Value r; r.type = VType::Quat; r.v[0] = x; r.v[1] = y; r.v[2] = z; r.v[3] = w; return r; }
};

// Declared parameter / return types, as written in docs and type stubs. A trailing '?' means optional, '[]' an array.
// Allowed: bool number string vec3 quat entity (an index, -1 or nil for none) any
struct Param {
    std::string name;
    std::string type;
};

enum class Avail { Always, PlayOnly, EditorOnly };

using ApiFn = std::function<bool(const std::vector<Value>& args, Value& out, std::string& error)>;

struct ApiFunction {
    std::string module;  // scene, world, audio, ...
    std::string name;
    std::string doc;     // required: the generated docs and the build check reject an empty one
    std::string since;   // api version that added it, e.g. "1.0"
    std::vector<Param> params;
    std::string returns; // "" for none
    Avail avail = Avail::Always;
    ApiFn fn;
};

// Registered at startup by each api_<area>.cpp. Lookup is by module + name; handles stay valid for the process lifetime.
class Registry {
public:
    static Registry& Get();

    void Add(ApiFunction f);
    const ApiFunction* Find(const std::string& module, const std::string& name) const;
    const std::deque<ApiFunction>& All() const { return m_functions; }

    // Calls by name with typed arguments; the same path Luau, the pipe and game.dll use.
    bool Call(const std::string& module, const std::string& name, const std::vector<Value>& args, Value& out, std::string& error) const;

    // Checks args against the declared signature (count, types, optional trailing ones). Returns false with a message
    // that names the function and the parameter.
    static bool CheckArgs(const ApiFunction& f, const std::vector<Value>& args, std::string& error);

    // Generated outputs, all from the table above.
    std::string ReferenceMarkdown() const;
    std::string LuauTypeDefs() const;
    std::string ListJson() const; // for the pipe (script.list) and the MCP tool list
    // Names of registrations that have no doc string; the build and the smoke test fail on any.
    std::vector<std::string> Undocumented() const;

private:
    std::deque<ApiFunction> m_functions; // a deque so pointers to entries stay valid as more are added
};

} // namespace tbd::script

// The engine services the api_*.cpp files call are the existing TbdGameHost table (src/game_api.h), so main.cpp needs no
// new glue: it passes GameHost(). New engine features register here instead of growing that frozen struct.
namespace tbd::script {
// Engine features that live in main.cpp and are not part of the frozen TbdGameHost table. main.cpp fills these in at startup
// (null = not available, the registered call then fails with a clear error). Add a field here instead of growing TbdGameHost.
struct EngineServices {
    bool (*setSkinDetailScale)(int entity, float scale) = nullptr; // character.set_skin_detail_scale
    bool (*setSkinPoreDepth)(int entity, float depth) = nullptr;   // character.set_skin_pore_depth
};
EngineServices& Services();
} // namespace tbd::script

struct TbdGameHost;
namespace tbd::script {
// Registers every built-in module against `host`. Safe to call again (replaces the earlier registration).
void RegisterBuiltinApi(const TbdGameHost* host);
} // namespace tbd::script
