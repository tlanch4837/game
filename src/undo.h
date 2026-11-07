// Engine-wide undo / redo (Ctrl+Z, Ctrl+Y or Ctrl+Shift+Z): one stack of named steps. Each step carries its own undo and redo.
// Who pushes: main.cpp's scene checkpoint (any entity edit: move / rotate / scale, rename, create, delete, properties; one step
// per finished mouse or key action) and the Character Creator (sliders, randomize, skin tone, hair strokes, fill / clear).
// A small popup at the viewport's bottom right says what was undone or redone ("Undid Move Head").
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace tbd::undo {

struct Step {
    std::string label;
    std::function<void()> undo, redo;
};

struct Stacks {
    std::vector<Step> done, undone;
    int absorb = 0; // > 0: the scene checkpoint takes the scene's next change as its new baseline without a step (a tool made it)
};
inline Stacks& S() { static Stacks s; return s; }

constexpr size_t kMaxSteps = 200;

inline void Push(const std::string& label, std::function<void()> undo, std::function<void()> redo) {
    Stacks& s = S();
    s.done.push_back({label, std::move(undo), std::move(redo)});
    if (s.done.size() > kMaxSteps) s.done.erase(s.done.begin());
    s.undone.clear();
}
// A tool changed the scene's entities itself and recorded (or did not want) its own step: the scene checkpoint must not add one.
inline void AbsorbSceneChange() { ++S().absorb; }

inline bool Undo(std::string* label) {
    Stacks& s = S();
    if (s.done.empty()) return false;
    Step st = std::move(s.done.back());
    s.done.pop_back();
    if (st.undo) st.undo();
    *label = st.label;
    s.undone.push_back(std::move(st));
    return true;
}
inline bool Redo(std::string* label) {
    Stacks& s = S();
    if (s.undone.empty()) return false;
    Step st = std::move(s.undone.back());
    s.undone.pop_back();
    if (st.redo) st.redo();
    *label = st.label;
    s.done.push_back(std::move(st));
    return true;
}

} // namespace tbd::undo

#if defined(_WIN32)
#include "undo_toast_win.h" // the Win32 popup; Windows-only callers (main.cpp, the Character Creator) use it
#endif
