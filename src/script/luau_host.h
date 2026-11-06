// Owns the Luau state: loads scripts from the project's Scripts folder, binds the registry as tbd.<module>.<name>, runs hooks.
// Main thread only. Scripts never see engine objects, only registry functions (api_registry.h).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct lua_State;

namespace tbd::script {

class LuauHost {
public:
    ~LuauHost() { Shutdown(); }

    // scriptsDir: the project's Scripts folder (UTF-8). log: where script output and errors go (engine log).
    bool Init(const std::string& scriptsDir, void (*log)(const char*));
    void Shutdown();
    bool Ready() const { return m_L != nullptr; }

    // Loads Scripts/manifest.luau (returns a list of script paths relative to Scripts) and each script in it.
    // A missing manifest is not an error: there are simply no global scripts.
    bool LoadManifest();
    // Loads one script file (relative to Scripts) as a global script. Errors are logged and false is returned.
    bool LoadScript(const std::string& relPath);

    // Attaches an entity script: a copy of the script (loaded on first use) with its own self and self.entity. props is the scene
    // file's text, "key=value;key=value", applied over the script's declared defaults.
    bool AddEntityScript(int entity, const std::string& relPath, const std::string& props);

    // Hot reload: re-reads every script loaded so far whose file changed. A script that fails to compile or run its top level
    // keeps its old code (the error is logged); one that works is swapped into every live instance, keeping each self.
    // Rate-limited to about 4 Hz unless force; force also compares file contents instead of timestamps. Returns reloads done.
    int PollReload(bool force = false);

    // What the inspector shows for a script: its declared props (name, type "number" | "bool" | "string", default as text and
    // an optional number range). Loads the script file first if it was not loaded. Empty when it is missing or declares none.
    struct PropInfo {
        std::string name, type, defaultText;
        bool hasMin = false, hasMax = false;
        double min = 0, max = 0;
    };
    std::vector<PropInfo> DeclaredProps(const std::string& relPath);
    // Changes whenever a script (re)loads, so a cached DeclaredProps can be refreshed.
    unsigned Generation() const { return m_generation; }
    // Script files under Scripts (relative, '/' separators), without manifest.luau and the tests folder.
    std::vector<std::string> ListScripts() const;
    bool ScriptExists(const std::string& relPath) const; // the file is there (path rules as for loading)

    // Play start / one frame / Play stop. Update runs on_update, then the coroutines, then on_late_update, and steps
    // on_fixed_update at 60 Hz from the time that has built up.
    void Start();
    void Update(float dt);
    void Stop();

    // Runs source text in the sandbox (script.eval, tests). Returns false with the error text in `result`.
    bool Eval(const std::string& source, const std::string& chunkName, std::string& result);

    // Wall-clock limit for one hook dispatch; a script that runs longer is aborted with an error.
    void SetTimeLimitMs(double ms) { m_limitMs = ms; }
    size_t MemoryBytes() const { return m_bytes; }
    void SetMemoryCapBytes(size_t cap) { m_cap = cap; }

    lua_State* State() { return m_L; }
    const std::string& ScriptsDir() const { return m_dir; }
    void Log(const char* text) const { if (m_log) m_log(text); }

    // Used by the allocator and interrupt callbacks.
    size_t m_bytes = 0, m_cap = 64u << 20;
    double m_deadline = 0; // seconds on the host clock; 0 = no limit active

private:
    bool ReadScript(const std::string& relPath, std::string& source);
    // Compiles and runs a script file's top level and hands the returned table to tbd.__define (new script or hot reload).
    bool LoadDef(const std::string& relPath);
    bool CallHook(const char* name, bool hasArg, double arg);
    bool Protected(int nargs, int nresults, const char* what); // pcall under the time limit

    lua_State* m_L = nullptr;
    std::string m_dir;
    void (*m_log)(const char*) = nullptr;
    double m_limitMs = 250.0;
    float m_fixedAccum = 0;
    struct Stamp { int64_t time = 0; size_t hash = 0; };
    std::map<std::string, Stamp> m_known; // every script path loaded, for hot reload
    double m_lastPoll = 0;
    unsigned m_generation = 0;
};

double HostClockSeconds();

} // namespace tbd::script
