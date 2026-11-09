// Link check for the game without the editor (src/game/game_host.cpp): references every host entry point so the linker
// must resolve everything the game calls. Built by link_game.bat; never meant to run (it would need a renderer).
#include <cstdio>
#include <memory>

#include "game/game_host.h"
#include "render/renderer.h"

int main(int argc, char**) {
    if (argc > 99) { // never true: the calls exist only so they are linked
        tbd::host::Init(L".");
        tbd::host::AttachRenderer(nullptr, 1, 1);
        tbd::host::StartCity();
        tbd::host::Resize(1, 1, 1.0f);
        tbd::host::PointerDown(tbd::host::Button::Left, 0, 0);
        tbd::host::PointerMove(0, 0);
        tbd::host::PointerUp(tbd::host::Button::Left, 0, 0);
        tbd::host::PointerCancel();
        tbd::host::Zoom(1.0f, 0, 0);
        tbd::host::KeyDown(0, false, 0);
        tbd::host::KeyUp(0);
        tbd::host::Frame();
        tbd::host::EnterBackground();
        tbd::host::Shutdown();
        return tbd::host::HasRenderer() ? 1 : 0;
    }
    std::printf("game_host links\n");
    return 0;
}
