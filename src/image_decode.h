// Decodes PNG / JPEG (anything Windows Imaging Component knows) from memory into top-down RGBA8.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <vector>

#pragma comment(lib, "windowscodecs.lib")

namespace tbd {

inline bool DecodeImage(const uint8_t* data, size_t size, std::vector<uint8_t>& rgba, int& width, int& height) {
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

} // namespace tbd
