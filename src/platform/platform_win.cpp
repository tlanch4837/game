// Windows implementation of src/platform/platform.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "platform.h"

#pragma comment(lib, "Synchronization.lib") // WaitOnAddress
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace tbd::platform {

int64_t Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

int64_t TickFrequency() {
    static const int64_t freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f.QuadPart; }();
    return freq;
}

void WaitOnAddress(const volatile void* address, const void* expected, std::size_t size, uint32_t timeoutMs) {
    ::WaitOnAddress(const_cast<volatile void*>(address), const_cast<void*>(expected), size, timeoutMs);
}

void WakeAddressAll(const void* address) { ::WakeByAddressAll(const_cast<void*>(address)); }

std::filesystem::path CacheRoot() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const std::filesystem::path exe = path;
    return exe.has_parent_path() ? exe.parent_path() : std::filesystem::path(L".");
}

FILE* OpenFile(const std::filesystem::path& path, const char* mode) {
    wchar_t wmode[8] = {};
    for (int i = 0; i < 7 && mode[i]; ++i) wmode[i] = (wchar_t)mode[i];
    FILE* f = nullptr;
    return _wfopen_s(&f, path.c_str(), wmode) == 0 ? f : nullptr;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool DecodeImage(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height) {
    using Microsoft::WRL::ComPtr;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(co); // RPC_E_CHANGED_MODE: COM is already up in another mode, which is fine
    bool ok = false;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> conv;
        UINT w = 0, h = 0;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->CreateStream(&stream)) &&
            SUCCEEDED(stream->InitializeFromMemory(const_cast<uint8_t*>(data), (DWORD)size)) &&
            SUCCEEDED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) &&
            SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && w > 0 && h > 0 &&
            SUCCEEDED(factory->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeCustom))) {
            rgba.resize((size_t)w * h * 4);
            ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)rgba.size(), rgba.data()));
            width = (int)w;
            height = (int)h;
        }
    }
    if (uninit) CoUninitialize();
    return ok;
}

// Key values are the virtual-key codes: KeyDownAsync / KeyDownSync are the exact calls the game made before.
static_assert((int)Key::MouseLeft == VK_LBUTTON && (int)Key::MouseRight == VK_RBUTTON && (int)Key::MouseMiddle == VK_MBUTTON);
static_assert((int)Key::Back == VK_BACK && (int)Key::Tab == VK_TAB && (int)Key::Return == VK_RETURN && (int)Key::Escape == VK_ESCAPE && (int)Key::Delete == VK_DELETE);
static_assert((int)Key::Shift == VK_SHIFT && (int)Key::Control == VK_CONTROL && (int)Key::Alt == VK_MENU && (int)Key::Space == VK_SPACE);
static_assert((int)Key::Left == VK_LEFT && (int)Key::Up == VK_UP && (int)Key::Right == VK_RIGHT && (int)Key::Down == VK_DOWN);
static_assert((int)Key::A == 'A' && (int)Key::Q == 'Q' && (int)Key::W == 'W' && (int)Key::Z == 'Z');

uint32_t GetEnvA(const char* name, char* buf, uint32_t size) { return GetEnvironmentVariableA(name, buf, size); }
uint32_t GetEnvW(const wchar_t* name, wchar_t* buf, uint32_t size) { return GetEnvironmentVariableW(name, buf, size); }
uint64_t TickCountMs() { return GetTickCount64(); }
static_assert(kMaxPath == MAX_PATH);
FILE* WOpen(const wchar_t* path, const wchar_t* mode) { return _wfopen(path, mode); }

double SecondsSinceProcessStart() {
    FILETIME created, a, b, c, nowFt;
    GetProcessTimes(GetCurrentProcess(), &created, &a, &b, &c);
    GetSystemTimeAsFileTime(&nowFt);
    const auto ticks = [](const FILETIME& f) { return ((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime; };
    return (double)(ticks(nowFt) - ticks(created)) / 1e7;
}

uint64_t ProcessPrivateBytes() {
    PROCESS_MEMORY_COUNTERS_EX pmc = {};
    pmc.cb = sizeof(pmc);
    return K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc)) ? pmc.PrivateUsage : 0;
}

bool KeyDownAsync(Key k) { return (GetAsyncKeyState((int)k) & 0x8000) != 0; }
bool KeyDownSync(Key k) { return (GetKeyState((int)k) & 0x8000) != 0; }
void SetKeyDown(Key, bool) {}

void LowerThreadPriority(std::thread& t) {
    SetThreadPriority((HANDLE)t.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
}

} // namespace tbd::platform

// The portable BMP headers (src/bmp_header.h) must match Windows' byte for byte.
#include "../bmp_header.h"
static_assert(sizeof(tbd::BmpFileHeader) == sizeof(BITMAPFILEHEADER) && sizeof(tbd::BmpInfoHeader) == sizeof(BITMAPINFOHEADER));
static_assert(offsetof(tbd::BmpFileHeader, bfOffBits) == offsetof(BITMAPFILEHEADER, bfOffBits));
static_assert(offsetof(tbd::BmpInfoHeader, biCompression) == offsetof(BITMAPINFOHEADER, biCompression) && tbd::kBmpRgb == BI_RGB);
