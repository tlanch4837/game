#pragma once
// Hot reload without restarting the engine (see docs/hot_reload.md):
//  - shaders: a change under src\shaders reloads them on Direct3D 12 at once and recompiles the SPIR-V in the background,
//    after which a Vulkan renderer is restarted in place (scene kept). F5 does the same by hand.
//  - gameplay: bin\game.dll (game\, build_game.bat) is copied, loaded, and swapped whenever it is rebuilt; a change under
//    game\ rebuilds it in the background. Its state lives in a block of memory the engine keeps across swaps.
// A failed compile or build keeps what is running and puts the compiler's errors in the log.
#include <windows.h>
#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include "game_api.h"
#include "render/renderer.h"

namespace tbd::hot {

// A folder tree watched for writes, settled for a moment before it counts (editors save in several steps).
class DirWatch {
public:
    ~DirWatch() { Close(); }
    void Open(const std::wstring& dir) {
        Close();
        handle_ = FindFirstChangeNotificationW(dir.c_str(), TRUE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
        if (handle_ == INVALID_HANDLE_VALUE) handle_ = nullptr;
    }
    void Close() {
        if (handle_) FindCloseChangeNotification(handle_);
        handle_ = nullptr;
    }
    bool Changed() {
        if (!handle_) return false;
        while (WaitForSingleObject(handle_, 0) == WAIT_OBJECT_0) {
            changedAt_ = GetTickCount64();
            if (!FindNextChangeNotification(handle_)) { Close(); break; }
        }
        if (changedAt_ && GetTickCount64() - changedAt_ > 400) {
            changedAt_ = 0;
            return true;
        }
        return false;
    }

private:
    HANDLE handle_ = nullptr;
    ULONGLONG changedAt_ = 0;
};

// A batch file run hidden on a thread, its output kept in a log file for when it fails.
class Job {
public:
    enum State { Idle, Running, Succeeded, Failed };
    ~Job() { if (thread_.joinable()) thread_.join(); }
    bool Busy() const { return state_ == Running; }
    bool Start(const std::wstring& batch, const std::wstring& args, const std::wstring& logFile) {
        if (Busy()) return false;
        if (thread_.joinable()) thread_.join();
        log_ = logFile;
        state_ = Running;
        thread_ = std::thread([this, cmd = L"cmd.exe /d /c \"\"" + batch + L"\" " + args + L" > \"" + logFile + L"\" 2>&1\""]() mutable {
            STARTUPINFOW si = {sizeof(si)};
            PROCESS_INFORMATION pi = {};
            DWORD code = 1;
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, INFINITE);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
            }
            state_ = code == 0 ? Succeeded : Failed;
        });
        return true;
    }
    // Succeeded or Failed once, then Idle again.
    State Take() {
        State s = state_;
        if (s == Succeeded || s == Failed) state_ = Idle;
        return s;
    }
    void LogOutput(const char* what) const {
        Log("%s failed:", what);
        if (FILE* f = _wfopen(log_.c_str(), L"rb")) {
            char line[1024];
            int shown = 0;
            while (fgets(line, sizeof(line), f) && shown < 40)
                if (strstr(line, "error") || strstr(line, "Error")) { line[strcspn(line, "\r\n")] = 0; Log("  %s", line); ++shown; }
            fclose(f);
        }
    }

private:
    std::thread thread_;
    std::atomic<State> state_{Idle};
    std::wstring log_;
};

inline std::wstring RootDirectory() { return ExeDirectory() + L"\\.."; }

inline bool FileStamp(const std::wstring& path, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    out = a.ftLastWriteTime;
    return true;
}

inline void CopyMatching(const std::wstring& from, const std::wstring& pattern, const std::wstring& to) {
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW((from + L"\\" + pattern).c_str(), &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do CopyFileW((from + L"\\" + fd.cFileName).c_str(), (to + L"\\" + fd.cFileName).c_str(), FALSE); while (FindNextFileW(find, &fd));
    FindClose(find);
}

// The shader half. Tick returns true when the Vulkan renderer should be restarted to pick up new SPIR-V.
class Shaders {
public:
    void Init() {
        watch_.Open(RootDirectory() + L"\\src\\shaders");
        canSpirv_ = GetEnvironmentVariableW(L"VULKAN_SDK", nullptr, 0) > 0 && GetFileAttributesW((RootDirectory() + L"\\spirv.bat").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    // What F5 and a file change both do. Direct3D 12 reloads now; the SPIR-V follows in the background.
    void Reload(Renderer* renderer, bool vulkan) {
        if (renderer && !vulkan) Log(renderer->ReloadShaders() ? "Shaders reloaded" : "Shader reload failed (see above); the running shaders stay");
        if (!canSpirv_) {
            if (vulkan) Log("Vulkan shader reload needs VULKAN_SDK set (for dxc) and spirv.bat next to build.bat");
            return;
        }
        if (job_.Busy()) { again_ = true; return; }
        const std::wstring scratch = RootDirectory() + L"\\obj\\spirv_reload";
        CreateDirectoryW((RootDirectory() + L"\\obj").c_str(), nullptr);
        CreateDirectoryW(scratch.c_str(), nullptr);
        job_.Start(RootDirectory() + L"\\spirv.bat", L"\"" + scratch + L"\"", RootDirectory() + L"\\obj\\spirv_reload.log");
        if (vulkan) Log("Vulkan: compiling shaders...");
    }
    bool Tick(Renderer* renderer, bool vulkan) {
        if (watch_.Changed()) Reload(renderer, vulkan);
        switch (job_.Take()) {
        case Job::Succeeded:
            CopyMatching(RootDirectory() + L"\\obj\\spirv_reload", L"*.spv", ExeDirectory() + L"\\shaders");
            if (again_) { again_ = false; Reload(nullptr, vulkan); return false; }
            return vulkan;
        case Job::Failed:
            job_.LogOutput("Vulkan shader compile");
            again_ = false;
            return false;
        default: return false;
        }
    }

private:
    DirWatch watch_;
    Job job_;
    bool canSpirv_ = false, again_ = false;
};

// Calls into the game behind SEH, so a crash in gameplay code unloads the DLL instead of the engine.
inline bool GuardedLoad(const TbdGameApi* a, const TbdGameHost* h, void* m, size_t n, int r) { __try { a->onLoad(h, m, n, r); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
inline bool GuardedPlay(const TbdGameApi* a, int p) { __try { if (a->onPlay) a->onPlay(p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
inline bool GuardedUpdate(const TbdGameApi* a, float dt, int p) { __try { a->update(dt, p); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }
inline bool GuardedUnload(const TbdGameApi* a) { __try { if (a->onUnload) a->onUnload(); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

// The gameplay DLL half.
class Game {
public:
    static constexpr size_t kMemory = 64ull << 20; // reserved; pages are only committed as the game touches them

    ~Game() { Shutdown(); }
    void Init(const TbdGameHost* host) {
        host_ = host;
        memory_ = VirtualAlloc(nullptr, kMemory, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        dll_ = ExeDirectory() + L"\\game.dll";
        hotDir_ = ExeDirectory() + L"\\hot";
        CreateDirectoryW(hotDir_.c_str(), nullptr);
        CleanHotDir();
        if (GetFileAttributesW((RootDirectory() + L"\\game").c_str()) != INVALID_FILE_ATTRIBUTES) watch_.Open(RootDirectory() + L"\\game");
        TryLoad();
    }
    void Shutdown() {
        Unload();
        if (memory_) VirtualFree(memory_, 0, MEM_RELEASE);
        memory_ = nullptr;
    }
    // Rebuilds bin\game.dll in the background; Tick swaps it in when it lands.
    void Rebuild() {
        if (build_.Busy()) { again_ = true; return; }
        const std::wstring batch = RootDirectory() + L"\\build_game.bat";
        if (GetFileAttributesW(batch.c_str()) == INVALID_FILE_ATTRIBUTES) { Log("Gameplay: build_game.bat is missing"); return; }
        build_.Start(batch, L"", RootDirectory() + L"\\obj\\game_build.log");
        Log("Gameplay: building game.dll...");
    }
    void Tick(float dt, bool playing) {
        if (watch_.Changed()) Rebuild();
        switch (build_.Take()) {
        case Job::Failed: build_.LogOutput("Gameplay build"); again_ = false; break;
        case Job::Succeeded: if (again_) { again_ = false; Rebuild(); } break;
        default: break;
        }
        FILETIME stamp;
        if (!build_.Busy() && FileStamp(dll_, stamp) && CompareFileTime(&stamp, &stamp_) != 0) TryLoad();
        if (api_ && playing != playing_) {
            if (!GuardedPlay(api_, playing)) Crashed("onPlay");
        }
        playing_ = playing;
        if (api_ && !GuardedUpdate(api_, dt, playing)) Crashed("update");
    }
    bool Loaded() const { return api_ != nullptr; }

private:
    void TryLoad() {
        FILETIME stamp;
        if (!FileStamp(dll_, stamp)) return;
        stamp_ = stamp; // tried once per build, whether or not it loads
        // A copy is loaded, so the linker can overwrite game.dll while it runs.
        const std::wstring copy = hotDir_ + L"\\game_" + std::to_wstring(++generation_) + L".dll";
        if (!CopyFileW(dll_.c_str(), copy.c_str(), FALSE)) { stamp_ = {}; return; } // still being written: next frame
        HMODULE module = LoadLibraryW(copy.c_str());
        auto entry = module ? (TbdGameGetApiFn)GetProcAddress(module, TBD_GAME_ENTRY) : nullptr;
        const TbdGameApi* api = entry ? entry(TBD_GAME_API_VERSION) : nullptr;
        if (!api || api->version != TBD_GAME_API_VERSION || !api->onLoad || !api->update) {
            Log("Gameplay: game.dll did not load (missing %s or API version is not %u); the running one stays", TBD_GAME_ENTRY, TBD_GAME_API_VERSION);
            if (module) FreeLibrary(module);
            DeleteFileW(copy.c_str());
            return;
        }
        const bool reloaded = everLoaded_;
        Unload();
        module_ = module;
        moduleFile_ = copy;
        api_ = api;
        everLoaded_ = true;
        if (!GuardedLoad(api_, host_, memory_, kMemory, reloaded)) { Crashed("onLoad"); return; }
        if (playing_ && !GuardedPlay(api_, 1)) { Crashed("onPlay"); return; }
        Log(reloaded ? "Gameplay: game.dll reloaded" : "Gameplay: game.dll loaded");
    }
    void Unload() {
        if (api_) GuardedUnload(api_);
        api_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
        if (!moduleFile_.empty()) DeleteFileW(moduleFile_.c_str());
        moduleFile_.clear();
    }
    void Crashed(const char* where) {
        Log("Gameplay: game.dll crashed in %s; it is unloaded until the next build", where);
        api_ = nullptr; // no onUnload: its state may be what crashed
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
        everLoaded_ = false; // the next one starts from clean memory
        if (memory_) { VirtualFree(memory_, kMemory, MEM_DECOMMIT); VirtualAlloc(memory_, kMemory, MEM_COMMIT, PAGE_READWRITE); }
    }
    void CleanHotDir() {
        WIN32_FIND_DATAW fd;
        HANDLE find = FindFirstFileW((hotDir_ + L"\\game_*").c_str(), &fd);
        if (find == INVALID_HANDLE_VALUE) return;
        do DeleteFileW((hotDir_ + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(find, &fd));
        FindClose(find);
    }

    const TbdGameHost* host_ = nullptr;
    void* memory_ = nullptr;
    std::wstring dll_, hotDir_, moduleFile_;
    HMODULE module_ = nullptr;
    const TbdGameApi* api_ = nullptr;
    FILETIME stamp_ = {};
    int generation_ = 0;
    bool everLoaded_ = false, playing_ = false, again_ = false;
    DirWatch watch_;
    Job build_;
};

} // namespace tbd::hot
