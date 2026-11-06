// Editor tools and their categories. Pure data and selection state; the panel that draws and
// clicks them is in tool_panel.cpp. Tools only become "active" for now: the viewport reads
// ToolSet::Active() once there is a scene to act on.
#pragma once
#include <functional>
#include <vector>

namespace tbd {

enum class ToolId { Select, Move, Rotate, Scale, Cube, Sphere, Plane, Tree, Rock, Pan, Orbit, Zoom, FaceMocap, FaceMocapPreview, FaceDrive, FaceDriveStop, FaceLive };
// FaceMocap* and FaceDrive* are action tools: clicking one runs a workflow instead of making it the active tool.

struct Tool {
    ToolId id;
    const wchar_t* name;
    wchar_t key; // keyboard shortcut (upper case), 0 = none
};

struct ToolCategory {
    const wchar_t* name;
    std::vector<Tool> tools;
    bool expanded = true;
};

class ToolSet {
public:
    ToolSet() {
        categories_ = {
            {L"Transform",
             {{ToolId::Select, L"Select", L'Q'},
              {ToolId::Move, L"Move", L'W'},
              {ToolId::Rotate, L"Rotate", L'E'},
              {ToolId::Scale, L"Scale", L'R'}}},
            {L"Create",
             {{ToolId::Cube, L"Cube", L'1'},
              {ToolId::Sphere, L"Sphere", L'2'},
              {ToolId::Plane, L"Plane", L'3'},
              {ToolId::Tree, L"Tree", L'4'},
              {ToolId::Rock, L"Rock", L'5'}}},
            {L"View",
             {{ToolId::Pan, L"Pan", L'H'},
              {ToolId::Orbit, L"Orbit", L'O'},
              {ToolId::Zoom, L"Zoom", L'Z'}}},
            {L"Mocap",
             {{ToolId::FaceMocap, L"Face from Video...", 0},
              {ToolId::FaceLive, L"Live Webcam", 0},
              {ToolId::FaceMocapPreview, L"Preview Clip...", 0},
              {ToolId::FaceDrive, L"Drive Selected...", 0},
              {ToolId::FaceDriveStop, L"Stop Driving", 0}},
             false},
        };
    }

    std::vector<ToolCategory>& Categories() { return categories_; }
    const std::vector<ToolCategory>& Categories() const { return categories_; }

    // Runs when an action tool is clicked in the panel; set by the engine.
    std::function<void(ToolId)> onAction;

    int openSet = 0; // category whose tools the right-hand panel shows

    ToolId Active() const { return active_; }
    void SetActive(ToolId id) { active_ = id; }

    const Tool* Find(ToolId id) const {
        for (const auto& c : categories_)
            for (const auto& t : c.tools)
                if (t.id == id) return &t;
        return nullptr;
    }

    const Tool* ActiveTool() const { return Find(active_); }

    // Activates the tool whose shortcut is `key` (upper case). Returns false if none matches.
    bool ActivateByKey(wchar_t key) {
        for (int i = 0; i < (int)categories_.size(); ++i)
            for (const auto& t : categories_[i].tools)
                if (t.key == key) {
                    active_ = t.id;
                    openSet = i;
                    return true;
                }
        return false;
    }

private:
    std::vector<ToolCategory> categories_;
    ToolId active_ = ToolId::Select;
};

} // namespace tbd
