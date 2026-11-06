#include "video_reader.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace tbd {

struct VideoReader::Impl {
    ComPtr<IMFMediaSource> cameraSource; // live capture only
    ComPtr<IMFSourceReader> reader;
    int srcW = 0, srcH = 0;
    int stride = 0;     // bytes per row of the decoded RGB32 buffer; negative = bottom-up
    int rotation = 0;   // degrees clockwise the stored picture must be turned to be upright
    bool started = false;
    ~Impl() {
        reader.Reset();
        if (cameraSource) cameraSource->Shutdown();
        cameraSource.Reset();
        if (started) MFShutdown();
    }
};

VideoReader::VideoReader() : impl_(new Impl) {}
VideoReader::~VideoReader() = default;

bool VideoReader::Open(const wchar_t* path) {
    Impl& s = *impl_;
    if (FAILED(MFStartup(MF_VERSION))) return false;
    s.started = true;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 1);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE); // lets the reader convert to RGB32
    if (FAILED(MFCreateSourceReaderFromURL(path, attrs.Get(), &s.reader))) return false;
    return Setup();
}

bool VideoReader::Setup() {
    Impl& s = *impl_;
    s.reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    s.reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    ComPtr<IMFMediaType> native;
    if (SUCCEEDED(s.reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &native))) {
        UINT32 rot = 0;
        if (SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_ROTATION, &rot))) s.rotation = (int)((rot % 360 + 360) % 360);
    }

    ComPtr<IMFMediaType> want;
    MFCreateMediaType(&want);
    want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (FAILED(s.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, want.Get()))) return false;

    ComPtr<IMFMediaType> cur;
    if (FAILED(s.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur))) return false;
    UINT32 w = 0, h = 0, n = 0, d = 1;
    if (FAILED(MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h) return false;
    s.srcW = (int)w;
    s.srcH = (int)h;
    s.stride = (int)(INT32)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, (UINT32)(w * 4));
    if (SUCCEEDED(MFGetAttributeRatio(cur.Get(), MF_MT_FRAME_RATE, &n, &d)) && d) fps_ = (double)n / d;

    PROPVARIANT dur;
    PropVariantInit(&dur);
    if (SUCCEEDED(s.reader->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &dur)) &&
        dur.vt == VT_UI8)
        duration_ = (double)dur.uhVal.QuadPart / 1e7;
    PropVariantClear(&dur);

    const bool swap = s.rotation == 90 || s.rotation == 270;
    width_ = swap ? s.srcH : s.srcW;
    height_ = swap ? s.srcW : s.srcH;
    return true;
}

namespace {
// Video capture devices, in the order MFEnumDeviceSources returns them. Caller releases the activates.
std::vector<ComPtr<IMFActivate>> EnumCameras() {
    std::vector<ComPtr<IMFActivate>> out;
    ComPtr<IMFAttributes> q;
    MFCreateAttributes(&q, 1);
    q->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr;
    UINT32 n = 0;
    if (SUCCEEDED(MFEnumDeviceSources(q.Get(), &devs, &n))) {
        for (UINT32 i = 0; i < n; ++i) out.push_back(ComPtr<IMFActivate>(devs[i])), devs[i]->Release();
        CoTaskMemFree(devs);
    }
    return out;
}
} // namespace

std::vector<std::wstring> VideoReader::ListCameras() {
    MFStartup(MF_VERSION);
    std::vector<std::wstring> names;
    for (auto& d : EnumCameras()) {
        WCHAR* name = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(d->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len))) {
            names.push_back(name);
            CoTaskMemFree(name);
        } else {
            names.push_back(L"Camera");
        }
    }
    MFShutdown();
    return names;
}

bool VideoReader::OpenCamera(int index, int maxWidth) {
    Impl& s = *impl_;
    if (FAILED(MFStartup(MF_VERSION))) return false;
    s.started = true;
    auto devs = EnumCameras();
    if (index < 0 || index >= (int)devs.size()) return false;
    if (FAILED(devs[index]->ActivateObject(IID_PPV_ARGS(&s.cameraSource)))) return false;

    ComPtr<IMFAttributes> attrs;
    MFCreateAttributes(&attrs, 2);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (FAILED(MFCreateSourceReaderFromMediaSource(s.cameraSource.Get(), attrs.Get(), &s.reader))) return false;

    // Prefer the biggest mode up to maxWidth wide that runs at 25+ fps: full HD frames only slow tracking down.
    UINT32 bestW = 0;
    ComPtr<IMFMediaType> best;
    ComPtr<IMFMediaType> t;
    for (DWORD i = 0; SUCCEEDED(s.reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t)); ++i) {
        UINT32 w = 0, h = 0, n = 0, d = 1;
        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &n, &d);
        if (w <= (UINT32)maxWidth && w > bestW && d && n / d >= 25) {
            bestW = w;
            best = t;
        }
        t.Reset();
    }
    if (best) s.reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, best.Get());
    return Setup();
}

bool VideoReader::Seek(double timeSec) {
    if (!impl_->reader) return false;
    PROPVARIANT pos;
    PropVariantInit(&pos);
    pos.vt = VT_I8;
    pos.hVal.QuadPart = (LONGLONG)(std::max(timeSec, 0.0) * 1e7);
    return SUCCEEDED(impl_->reader->SetCurrentPosition(GUID_NULL, pos));
}

bool VideoReader::ReadFrame(std::vector<uint8_t>& bgra, double& timeSec) {
    Impl& s = *impl_;
    if (!s.reader) return false;
    for (;;) {
        DWORD flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(s.reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, &flags, &ts, &sample)))
            return false;
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) return false;
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) { // the decoder settled on its real row layout
            ComPtr<IMFMediaType> cur;
            if (SUCCEEDED(s.reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
                s.stride = (int)(INT32)MFGetAttributeUINT32(cur.Get(), MF_MT_DEFAULT_STRIDE, (UINT32)s.stride);
        }
        if (!sample) continue; // stream gap

        ComPtr<IMFMediaBuffer> buf;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return false;
        // The frame's own row pitch: a 1080-wide video is often decoded into 1088-wide rows, whatever the media type says.
        ComPtr<IMF2DBuffer> buf2d;
        buf.As(&buf2d);
        BYTE* first = nullptr; // the top row
        LONG pitch = 0;
        BYTE* data = nullptr;
        DWORD len = 0;
        const bool locked2d = buf2d && SUCCEEDED(buf2d->Lock2D(&first, &pitch));
        if (!locked2d) {
            if (FAILED(buf->Lock(&data, nullptr, &len))) return false;
            pitch = s.stride;
            first = s.stride < 0 ? data + (size_t)(s.srcH - 1) * -s.stride : data;
        }

        const int sw = s.srcW, sh = s.srcH;
        bgra.resize((size_t)width_ * height_ * 4);
        auto srcPx = [&](int x, int y) -> const uint32_t* {
            return reinterpret_cast<const uint32_t*>(first + (ptrdiff_t)y * pitch) + x;
        };
        uint32_t* out = reinterpret_cast<uint32_t*>(bgra.data());
        if (locked2d || (size_t)std::abs(s.stride) * sh <= len) {
            for (int y = 0; y < height_; ++y)
                for (int x = 0; x < width_; ++x) {
                    int sx = x, sy = y; // map upright (x,y) back to the stored picture
                    switch (s.rotation) {
                    case 90: sx = y; sy = sh - 1 - x; break;
                    case 180: sx = sw - 1 - x; sy = sh - 1 - y; break;
                    case 270: sx = sw - 1 - y; sy = x; break;
                    default: break;
                    }
                    out[(size_t)y * width_ + x] = *srcPx(sx, sy) | 0xFF000000u;
                }
        }
        if (locked2d) buf2d->Unlock2D();
        else buf->Unlock();
        timeSec = (double)ts / 1e7;
        return true;
    }
}

} // namespace tbd
