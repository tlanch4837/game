// Windows implementation of src/platform/platform.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
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

void LowerThreadPriority(std::thread& t) {
    SetThreadPriority((HANDLE)t.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
}

} // namespace tbd::platform
