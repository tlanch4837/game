// Scene undo for the editor (included by main.cpp, after DeleteSelected): a checkpoint once per frame while no mouse button is
// held and nothing is being dragged. When the entities differ from the last checkpoint, one step is pushed with a name from the
// difference ("Move Head", "Delete Cube", "Edit 3 entities"); a quick run of the same edit on the same entity (typing in a field)
// merges into one step. Undo puts the old entities back; only the entities whose mesh changed are made again, and inserts and
// erases go through the same bookkeeping as Delete (meshes, world streaming).

namespace scene_undo {

struct State {
    std::vector<::tbd::Entity> base; // the scene as of the last checkpoint
    bool have = false;
    std::string lastLabel;
    double lastTime = 0;
};
inline State& S() { static State s; return s; }

inline bool SameQuat(const ::tbd::Quat& a, const ::tbd::Quat& b) { return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w; }
inline bool SameVec(const ::tbd::Vec3& a, const ::tbd::Vec3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
inline const ::tbd::Quat& Rot(const ::tbd::Entity& e) { return e.faceClip.empty() ? e.rotation : e.faceBase; } // driven: animated
inline bool SameIdentity(const ::tbd::Entity& a, const ::tbd::Entity& b) { return a.name == b.name && a.mesh == b.mesh && a.meshPath == b.meshPath; }
inline bool SameNpc(const ::tbd::NpcInfo& a, const ::tbd::NpcInfo& b) {
    if (a.enabled != b.enabled || a.routine != b.routine || a.wanderRadius != b.wanderRadius || a.reactRadius != b.reactRadius ||
        a.hasIndoors != b.hasIndoors || !SameVec(a.indoors, b.indoors) || a.patrol.size() != b.patrol.size()) return false;
    for (size_t i = 0; i < a.patrol.size(); ++i) if (!SameVec(a.patrol[i], b.patrol[i])) return false;
    return true;
}
inline bool SameScripts(const ::tbd::Entity& a, const ::tbd::Entity& b) {
    if (a.scripts.size() != b.scripts.size()) return false;
    for (size_t i = 0; i < a.scripts.size(); ++i) if (a.scripts[i].path != b.scripts[i].path || a.scripts[i].props != b.scripts[i].props) return false;
    return true;
}
// What changed on one entity, as a verb ("" = nothing).
inline std::string Change(const ::tbd::Entity& a, const ::tbd::Entity& b) {
    if (a.name != b.name) return "Rename";
    if (a.mesh != b.mesh || a.meshPath != b.meshPath) return "Change mesh of";
    if (!SameVec(a.position, b.position)) return "Move";
    if (!SameQuat(Rot(a), Rot(b))) return "Rotate";
    if (!SameVec(a.scale, b.scale)) return "Scale";
    if (!SameVec(a.color, b.color)) return "Recolour";
    if (a.faceClip != b.faceClip || a.animClip != b.animClip) return "Change clip of";
    if (!SameScripts(a, b)) return "Edit scripts of";
    if (!SameNpc(a.npc, b.npc)) return "Edit NPC";
    return "";
}

inline std::string Describe(const std::vector<::tbd::Entity>& from, const std::vector<::tbd::Entity>& to) {
    if (to.size() == from.size() + 1 || to.size() == from.size() - 1) {
        const bool added = to.size() > from.size();
        const std::vector<::tbd::Entity>& big = added ? to : from;
        const std::vector<::tbd::Entity>& fewer = added ? from : to;
        size_t k = 0;
        while (k < fewer.size() && SameIdentity(fewer[k], big[k])) ++k;
        return std::string(added ? "Create " : "Delete ") + big[k].name;
    }
    if (to.size() != from.size()) return to.size() > from.size() ? "Create " + std::to_string(to.size() - from.size()) + " entities"
                                                                  : "Delete " + std::to_string(from.size() - to.size()) + " entities";
    int changed = 0;
    std::string what;
    for (size_t i = 0; i < to.size(); ++i) {
        const std::string c = Change(from[i], to[i]);
        if (c.empty()) continue;
        if (++changed == 1) what = c + " " + (c == "Rename" ? from[i].name : to[i].name);
    }
    if (changed > 1) return "Edit " + std::to_string(changed) + " entities";
    return what;
}

inline bool Equal(const std::vector<::tbd::Entity>& a, const std::vector<::tbd::Entity>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!SameIdentity(a[i], b[i]) || !Change(a[i], b[i]).empty()) return false;
    return true;
}

// The scene's entities become `target`: erases and inserts by identity, then every field; meshes only where needed.
inline void Restore(const std::vector<::tbd::Entity>& target) {
    CompleteSceneLoad();
    auto& cur = g_engine.scene.entities;
    auto& meshes = g_engine.meshes;
    const bool gpu = g_engine.renderer != nullptr;
    size_t k = 0;
    while (k < target.size() || k < cur.size()) {
        if (k < target.size() && k < cur.size() && SameIdentity(cur[k], target[k])) { ++k; continue; }
        const bool erase = k < cur.size() && (k >= target.size() || (k + 1 < cur.size() && SameIdentity(cur[k + 1], target[k])) || cur.size() > target.size());
        if (erase) {
            if (gpu && k < meshes.size()) DestroyEntityMesh(meshes[k]);
            if (k < meshes.size()) meshes.erase(meshes.begin() + (std::ptrdiff_t)k);
            cur.erase(cur.begin() + (std::ptrdiff_t)k);
            g_stream.OnEntityErased(g_engine.renderer.get(), (int)k);
        } else {
            cur.insert(cur.begin() + (std::ptrdiff_t)k, target[k]);
            meshes.insert(meshes.begin() + (std::ptrdiff_t)std::min(k, meshes.size()), gpu ? UploadEntityMesh(target[k]) : ::tbd::MeshId{});
            g_stream.OnEntityInserted((int)k);
            ++k;
        }
    }
    for (size_t i = 0; i < cur.size(); ++i) {
        const bool remesh = cur[i].mesh != target[i].mesh || cur[i].meshPath != target[i].meshPath;
        cur[i] = target[i];
        if (remesh && gpu && i < meshes.size()) { DestroyEntityMesh(meshes[i]); meshes[i] = UploadEntityMesh(cur[i]); }
    }
    if (g_engine.scene.selected >= (int)cur.size()) g_engine.scene.selected = -1;
    S().base = cur;
    S().have = true;
}

inline double NowSeconds() { return (double)GetTickCount64() / 1000.0; }

// Once per frame. Quiet while a button is held, a drag runs, a scene loads, or Play owns the scene.
inline void Checkpoint() {
    State& s = S();
    if (g_play.active || SceneLoadActive()) { s.have = false; return; }
    if (g_engine.drag != Drag::None || tbd::platform::KeyDownAsync(tbd::platform::Key::MouseLeft) || tbd::platform::KeyDownAsync(tbd::platform::Key::MouseRight) || tbd::platform::KeyDownAsync(tbd::platform::Key::MouseMiddle)) return;
    const auto& cur = g_engine.scene.entities;
    if (!s.have) { s.base = cur; s.have = true; ::tbd::undo::S().absorb = 0; return; }
    if (Equal(s.base, cur)) { ::tbd::undo::S().absorb = 0; return; }
    if (::tbd::undo::S().absorb > 0) { ::tbd::undo::S().absorb = 0; s.base = cur; return; } // a tool made this change and keeps its own step
    const std::string label = Describe(s.base, cur);
    const double now = NowSeconds();
    auto& done = ::tbd::undo::S().done;
    if (label == s.lastLabel && now - s.lastTime < 1.0 && !done.empty() && done.back().label == label && ::tbd::undo::S().undone.empty()) {
        // the same edit again right away (typing a value): one step, its redo moves to the newest state
        auto after = std::make_shared<std::vector<::tbd::Entity>>(cur);
        done.back().redo = [after] { Restore(*after); };
    } else {
        auto before = std::make_shared<std::vector<::tbd::Entity>>(s.base), after = std::make_shared<std::vector<::tbd::Entity>>(cur);
        ::tbd::undo::Push(label, [before] { Restore(*before); }, [after] { Restore(*after); });
    }
    s.lastLabel = label;
    s.lastTime = now;
    s.base = cur;
}

// Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z from any window of the engine, except a text field (it keeps its own undo) and Play.
inline bool Key(const MSG& msg) {
    if (msg.message != WM_KEYDOWN || g_play.active) return false;
    if (!tbd::platform::KeyDownSync(tbd::platform::Key::Control) || tbd::platform::KeyDownSync(tbd::platform::Key::Alt)) return false;
    const bool shift = tbd::platform::KeyDownSync(tbd::platform::Key::Shift);
    const bool undo = msg.wParam == 'Z' && !shift, redo = msg.wParam == 'Y' || (msg.wParam == 'Z' && shift);
    if (!undo && !redo) return false;
    wchar_t cls[32] = {};
    if (msg.hwnd && GetClassNameW(msg.hwnd, cls, 32) && (_wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"RichEdit20W") == 0)) return false;
    Checkpoint(); // anything not yet recorded is a step first
    std::string label;
    const bool ok = undo ? ::tbd::undo::Undo(&label) : ::tbd::undo::Redo(&label);
    S().lastLabel.clear();
    if (ok) S().base = g_engine.scene.entities; // the step restored the scene (or a tool's state); nothing new to record
    RECT area = {};
    const HWND host = g_engine.viewportWindow ? g_engine.viewportWindow : g_engine.window;
    GetWindowRect(host, &area);
    ::tbd::undo::Toast(g_engine.window, area, ok ? std::string(undo ? "Undid " : "Redid ") + label : std::string(undo ? "Nothing to undo" : "Nothing to redo"));
    return true;
}

} // namespace scene_undo
