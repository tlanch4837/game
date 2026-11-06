// Headless script runner: tbd_script_test.exe. No window, no GPU, no scene file needed. It is the fast feedback loop for scripts and
// for the registry: a fake world (entities, a player, the sun hour) stands behind the same TbdGameHost table the engine fills.
//
//   tbd_script_test.exe --scripts <Scripts dir> --script-test <file.luau>     run a test script, exit 0 on pass
//   tbd_script_test.exe --dump-docs <out.md> --dump-types <out.d.luau>         write the generated reference and type stubs
//   tbd_script_test.exe --list-json                                             the registry as JSON (what script.list returns)
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../game_api.h"
#include "../scene_io.h"
#include "api_registry.h"
#include "luau_host.h"
#include "script_remote.h"

using namespace tbd::script;

namespace {

struct FakeEntity {
    std::string name;
    float p[3], q[4], s[3];
};
std::vector<FakeEntity> g_entities;
bool g_hasPlayer = false;
float g_player[3];
float g_hour = 12;
int g_failures = 0, g_checks = 0;
LuauHost g_host;
float g_lastSkinScale = -1; // what the stubbed character.set_skin_detail_scale last received
std::string g_marker;
tbd::Scene g_scene;       // from --scene: its entities fill the fake world and its S| scripts attach at tbd.test.play()
bool g_haveScene = false;
std::string g_scriptsDir;
std::vector<std::string> g_logs;

void HostLog(const char* text) { g_logs.push_back(text); printf("[log] %s\n", text); fflush(stdout); }

TbdGameHost MakeHost() {
    TbdGameHost h = {};
    h.version = TBD_GAME_API_VERSION;
    h.log = HostLog;
    h.entityCount = [] { return (int)g_entities.size(); };
    h.findEntity = [](const char* n) { for (size_t i = 0; i < g_entities.size(); ++i) if (g_entities[i].name == n) return (int)i; return -1; };
    h.entityName = [](int i) { return i >= 0 && i < (int)g_entities.size() ? g_entities[i].name.c_str() : ""; };
    h.getTransform = [](int i, float p[3], float q[4], float s[3]) {
        if (i < 0 || i >= (int)g_entities.size()) return;
        memcpy(p, g_entities[i].p, 12); memcpy(q, g_entities[i].q, 16); memcpy(s, g_entities[i].s, 12);
    };
    h.setTransform = [](int i, const float p[3], const float q[4], const float s[3]) {
        if (i < 0 || i >= (int)g_entities.size()) return;
        memcpy(g_entities[i].p, p, 12); memcpy(g_entities[i].q, q, 16); memcpy(g_entities[i].s, s, 12);
    };
    h.keyDown = [](int) { return 0; };
    h.playerFeet = [](float p[3]) { if (!g_hasPlayer) return 0; memcpy(p, g_player, 12); return 1; };
    h.getHour = [] { return g_hour; };
    h.setHour = [](float x) { g_hour = x; };
    h.playSound = [](const char* n, const float*, float, float) { printf("[sound] %s\n", n); };
    return h;
}

void RegTest(const char* name, const char* doc, std::vector<Param> params, const char* returns, ApiFn fn) {
    ApiFunction f;
    f.module = "test";
    f.name = name;
    f.since = "1.0";
    f.doc = doc;
    f.params = std::move(params);
    f.returns = returns;
    f.fn = std::move(fn);
    Registry::Get().Add(std::move(f));
}

void RegisterTestApi() {
    RegTest("check", "Records a pass or fail. A failure is printed with the message and makes the exit code nonzero.", {{"ok", "bool"}, {"message", "string?"}}, "",
            [](auto& a, Value&, std::string&) {
                ++g_checks;
                if (!a[0].b) { ++g_failures; printf("FAIL: %s\n", a.size() > 1 ? a[1].s.c_str() : "check failed"); }
                return true;
            });
    RegTest("check_near", "Passes when two numbers differ by at most eps.", {{"actual", "number"}, {"expected", "number"}, {"eps", "number"}, {"message", "string?"}}, "",
            [](auto& a, Value&, std::string&) {
                ++g_checks;
                if (std::fabs(a[0].n - a[1].n) > a[2].n) {
                    ++g_failures;
                    printf("FAIL: %s (actual %.5f, expected %.5f, eps %.5f)\n", a.size() > 3 ? a[3].s.c_str() : "check_near", a[0].n, a[1].n, a[2].n);
                }
                return true;
            });
    RegTest("add_entity", "Adds an entity to the fake world at a position. Returns its index.", {{"name", "string"}, {"position", "vec3"}}, "number",
            [](auto& a, Value& out, std::string&) {
                FakeEntity e = {a[0].s, {a[1].v[0], a[1].v[1], a[1].v[2]}, {0, 0, 0, 1}, {1, 1, 1}};
                g_entities.push_back(e);
                out = Value::Num((double)g_entities.size() - 1);
                return true;
            });
    RegTest("set_player", "Places the walking player's feet. Call with no argument to remove the player.", {{"feet", "vec3?"}}, "", [](auto& a, Value&, std::string&) {
        g_hasPlayer = !a.empty() && a[0].type == VType::Vec3;
        if (g_hasPlayer) memcpy(g_player, a[0].v, 12);
        return true;
    });
    RegTest("play", "Loads Scripts/manifest.luau, starts Play and runs on_start of every global script.", {}, "", [](auto&, Value&, std::string&) {
        g_host.LoadManifest();
        if (g_haveScene)
            for (size_t i = 0; i < g_scene.entities.size(); ++i)
                for (const tbd::EntityScript& sc : g_scene.entities[i].scripts) g_host.AddEntityScript((int)i, sc.path, sc.props);
        g_host.Start();
        return true;
    });
    RegTest("load", "Loads one script file (relative to Scripts) as a global script.", {{"path", "string"}}, "bool", [](auto& a, Value& out, std::string&) {
        out = Value::Bool(g_host.LoadScript(a[0].s));
        return true;
    });
    RegTest("start", "Runs on_start on every loaded script (use after tbd.test.load instead of tbd.test.play).", {}, "", [](auto&, Value&, std::string&) {
        g_host.Start();
        return true;
    });
    RegTest("step", "Advances the game by this many seconds at a fixed 1/60 s step.", {{"seconds", "number"}}, "", [](auto& a, Value&, std::string&) {
        const int n = (int)std::lround(a[0].n * 60.0);
        for (int i = 0; i < n; ++i) g_host.Update(1.0f / 60.0f);
        return true;
    });
    RegTest("attach", "Attaches an entity script to a fake-world entity; props is \"key=value;key=value\".", {{"entity", "entity"}, {"path", "string"}, {"props", "string?"}}, "bool",
            [](auto& a, Value& out, std::string&) {
                out = Value::Bool(g_host.AddEntityScript((int)a[0].n, a[1].s, a.size() > 2 ? a[2].s : std::string()));
                return true;
            });
    RegTest("write_script", "Writes a file under the Scripts folder (a test edits a script, then calls tbd.test.reload).", {{"path", "string"}, {"text", "string"}}, "",
            [](auto& a, Value&, std::string& err) {
                if (a[0].s.find("..") != std::string::npos || a[0].s.find(':') != std::string::npos) { err = "bad path"; return false; }
                std::ofstream out(g_scriptsDir + "/" + a[0].s, std::ios::binary);
                out << a[1].s;
                return (bool)out;
            });
    RegTest("remove_script", "Deletes a file under the Scripts folder.", {{"path", "string"}}, "", [](auto& a, Value&, std::string& err) {
        if (a[0].s.find("..") != std::string::npos || a[0].s.find(':') != std::string::npos) { err = "bad path"; return false; }
        std::remove((g_scriptsDir + "/" + a[0].s).c_str());
        return true;
    });
    RegTest("remote", "Runs one pipe line (for example script.call|world|set_hour|[7]) through the same code the editor's pipe uses. Returns 'OK <reply>' or 'ERR <reply>'.",
            {{"line", "string"}}, "string", [](auto& a, Value& out, std::string&) {
                std::vector<std::string> parts(1);
                for (char c : a[0].s) { if (c == '|') parts.emplace_back(); else parts.back() += c; }
                std::string reply;
                const bool ok = tbd::script::RemoteScriptCommand(g_host, parts, reply);
                out = Value::Str((ok ? "OK " : "ERR ") + reply);
                return true;
            });
    RegTest("props", "The declared props of a script as 'name:type:default[:min][:max]' joined by ';' (what the inspector shows).", {{"path", "string"}}, "string",
            [](auto& a, Value& out, std::string&) {
                std::string text;
                for (const auto& p : g_host.DeclaredProps(a[0].s)) {
                    text += (text.empty() ? "" : ";") + p.name + ":" + p.type + ":" + p.defaultText;
                    if (p.hasMin) text += ":min" + std::to_string((int)p.min);
                    if (p.hasMax) text += ":max" + std::to_string((int)p.max);
                }
                out = Value::Str(text);
                return true;
            });
    RegTest("list_scripts", "Script files the inspector's Add menu offers, joined by ';'.", {}, "string", [](auto&, Value& out, std::string&) {
        std::string text;
        for (const std::string& s : g_host.ListScripts()) text += (text.empty() ? "" : ";") + s;
        out = Value::Str(text);
        return true;
    });
    RegTest("script_exists", "Whether a script file exists under Scripts.", {{"path", "string"}}, "bool", [](auto& a, Value& out, std::string&) {
        out = Value::Bool(g_host.ScriptExists(a[0].s));
        return true;
    });
    RegTest("last_skin_scale", "The scale the stubbed engine last received from character.set_skin_detail_scale.", {}, "number", [](auto&, Value& out, std::string&) {
        out = Value::Num(g_lastSkinScale);
        return true;
    });
    RegTest("reload", "Runs the hot-reload check now. Returns how many scripts were reloaded.", {}, "number", [](auto&, Value& out, std::string&) {
        out = Value::Num(g_host.PollReload(true));
        return true;
    });
    RegTest("set_marker", "Records a string a test can read back with tbd.test.marker.", {{"text", "string"}}, "", [](auto& a, Value&, std::string&) {
        g_marker = a[0].s;
        return true;
    });
    RegTest("marker", "The last string recorded with tbd.test.set_marker.", {}, "string", [](auto&, Value& out, std::string&) {
        out = Value::Str(g_marker);
        return true;
    });
    RegTest("log_count", "How many log lines so far contain this text.", {{"text", "string"}}, "number", [](auto& a, Value& out, std::string&) {
        int n = 0;
        for (const std::string& l : g_logs) if (l.find(a[0].s) != std::string::npos) ++n;
        out = Value::Num(n);
        return true;
    });
    RegTest("eval_fails", "True when running this source raises an error.", {{"source", "string"}}, "bool", [](auto& a, Value& out, std::string&) {
        std::string r;
        out = Value::Bool(!g_host.Eval(a[0].s, "=eval_fails", r));
        return true;
    });
    RegTest("stop", "Stops Play: on_stop runs, coroutines are cancelled.", {}, "", [](auto&, Value&, std::string&) {
        g_host.Stop();
        return true;
    });
}

bool WriteFile(const char* path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    out << text;
    return (bool)out;
}

} // namespace

int main(int argc, char** argv) {
    std::string scripts, testFile, docsOut, typesOut, scenePath, roundtrip;
    bool listJson = false, smoke = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--scripts") scripts = next();
        else if (a == "--script-test") testFile = next();
        else if (a == "--dump-docs") docsOut = next();
        else if (a == "--dump-types") typesOut = next();
        else if (a == "--scene") scenePath = next();
        else if (a == "--roundtrip") roundtrip = next();
        else if (a == "--list-json") listJson = true;
        else if (a == "--smoke") smoke = true;
        else { fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }
    static TbdGameHost host = MakeHost();
    Services().setSkinDetailScale = [](int entity, float scale) { g_lastSkinScale = scale; return entity >= 0 && entity < (int)g_entities.size(); };
    RegisterBuiltinApi(&host);
    RegisterTestApi();

    Registry& reg = Registry::Get();
    const auto bad = reg.Undocumented();
    if (!bad.empty()) {
        for (const auto& n : bad) printf("FAIL: tbd.%s has no doc string\n", n.c_str());
        return 1;
    }
    if (!docsOut.empty() && !WriteFile(docsOut.c_str(), reg.ReferenceMarkdown())) return 2;
    if (!typesOut.empty() && !WriteFile(typesOut.c_str(), reg.LuauTypeDefs())) return 2;
    if (listJson) printf("%s\n", reg.ListJson().c_str());
    g_scriptsDir = scripts;
    if (!roundtrip.empty()) { // a scene must survive load -> save -> load with its scripts intact
        tbd::Scene a, b;
        tbd::TerrainParams ta, tb;
        std::string text;
        if (!tbd::ReadAllText(std::wstring(roundtrip.begin(), roundtrip.end()), text) || !tbd::ParseScene(text, a, ta)) { printf("FAIL: cannot read %s\n", roundtrip.c_str()); return 1; }
        const std::string again = tbd::SerializeScene(a, ta);
        if (!tbd::ParseScene(again, b, tb) || a.entities.size() != b.entities.size()) { printf("FAIL: scene did not reload\n"); return 1; }
        int attached = 0, bad = 0;
        for (size_t i = 0; i < a.entities.size(); ++i) {
            attached += (int)a.entities[i].scripts.size();
            if (a.entities[i].scripts.size() != b.entities[i].scripts.size()) { ++bad; continue; }
            for (size_t k = 0; k < a.entities[i].scripts.size(); ++k)
                if (a.entities[i].scripts[k].path != b.entities[i].scripts[k].path || a.entities[i].scripts[k].props != b.entities[i].scripts[k].props) ++bad;
        }
        printf("roundtrip: %d entities, %d script(s), %d mismatch(es)\n", (int)a.entities.size(), attached, bad);
        if (bad || again != tbd::SerializeScene(b, tb)) { printf("FAIL: scene text changed on the second save\n"); return 1; }
        if (testFile.empty() && !smoke) return 0;
    }
    if (!scenePath.empty()) {
        tbd::TerrainParams t;
        if (!tbd::LoadSceneFile(std::wstring(scenePath.begin(), scenePath.end()), g_scene, t)) { printf("FAIL: cannot load scene %s\n", scenePath.c_str()); return 1; }
        g_haveScene = true;
        for (const tbd::Entity& e : g_scene.entities)
            g_entities.push_back({e.name, {e.position.x, e.position.y, e.position.z}, {e.rotation.x, e.rotation.y, e.rotation.z, e.rotation.w}, {e.scale.x, e.scale.y, e.scale.z}});
    }
    if (testFile.empty() && !smoke) return 0;

    if (!g_host.Init(scripts, HostLog)) { printf("FAIL: Luau host did not start\n"); return 1; }
    if (smoke) { // M0: run a line of Luau and print the result
        std::string out;
        const bool ok = g_host.Eval("print('hi from Luau ' .. _VERSION)\nreturn 1 + 1", "=smoke", out);
        printf("smoke: %s (%s)\n", ok ? "ok" : "FAILED", out.c_str());
        if (!ok) return 1;
    }
    if (!testFile.empty()) {
        std::ifstream in(testFile, std::ios::binary);
        if (!in) { printf("FAIL: cannot read %s\n", testFile.c_str()); return 1; }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string result;
        if (!g_host.Eval(ss.str(), "=" + testFile, result)) { printf("FAIL: %s\n", result.c_str()); return 1; }
        printf("%s: %d check(s), %d failure(s)\n", testFile.c_str(), g_checks, g_failures);
        if (g_checks == 0) { printf("FAIL: the test made no checks\n"); return 1; }
    }
    return g_failures ? 1 : 0;
}
