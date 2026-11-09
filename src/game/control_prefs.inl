// [controls] Settings > Controls in saves\settings.ini: the ctl_* keys of game/control_settings.h (only the values that differ
// from the defaults are written). Included by glass_dock_host.inl inside namespace gdock, after dock_news_stats.inl; the
// dock's LoadGuiPrefs / SaveGuiPrefs call these, and the CS2 HUD (game_core.inl) calls them on its own.

std::wstring SettingsFilePath(); // glass_dock_host.inl
bool Touch();                    // glass_dock_host.inl

inline std::string ReadSettingsText() {
    std::string text;
    if (FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"rb")) {
        char buf[512];
        for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
        fclose(f);
    }
    return text;
}

// The ctl_* keys into tbd::controls::g (a missing file or key: the default), and the dock's page onto it.
void LoadControlPrefs() {
    g_model.controls = &tbd::controls::g;
    g_model.touchFirst = Touch();
    tbd::controls::Parse(ReadSettingsText(), tbd::controls::g);
}

// Rewrites settings.ini with the current ctl_* keys (every other line kept).
void SaveControlPrefs() {
    const std::string text = tbd::controls::Merge(ReadSettingsText(), tbd::controls::g);
    FILE* f = tbd::platform::WOpen(SettingsFilePath().c_str(), L"wb");
    if (!f) return;
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
}

// The CS2 HUD's Options > Controls (gui/city_hud.h shows the page; it keeps no settings file of its own).
void LegacyControlsFrame(tbd::gui::CityHud& h) {
    static bool loaded = false;
    if (!loaded) loaded = true, LoadControlPrefs();
    if (h.controlsSave) h.controlsSave = false, SaveControlPrefs();
}

// The camera gizmo (gui/camera_gizmo.h) at the top left, both HUDs: an orbit camera only, never over a modal page (the
// callers check those). Its ring drag turns at Settings > Controls' twist speed (touch) or rotate speed (mouse).
tbd::gui::gizmo::State g_gizmo;
float g_gizmoTopPx = 0; // where the top-left stats text ends (pixels; DrawGameGui): the gizmo sits below it
void CameraGizmo(const tbd::gui::SafeInsets& safe) {
    tbd::gui::SafeInsets sa = safe;
    if (g_gui.Scale() > 0) sa.top = std::max(sa.top, g_gizmoTopPx / g_gui.Scale());
    if (g_play.active || g_engine.drag == Drag::Fly) return; // any orbit camera (the city's, or the editor scene's)
    const tbd::controls::Settings& c = tbd::controls::g;
    const bool touch = Touch();
    g_model.touchFirst = touch; // [controls] the page's row order follows the interface switch
    tbd::gui::gizmo::Params p;
    p.rotateSpeed = touch ? (c.twistSpeed > 0.0f ? c.twistSpeed : 1.0f) : c.mouseRotate;
    p.snap = c.twistSnap;
    p.tapMaxSec = c.tapMaxSec;
    p.slopPt = touch ? c.tapSlopPt : 4.0f;
    tbd::gui::gizmo::Frame(g_gui, g_gizmo, g_engine.camera, sa, p);
}
