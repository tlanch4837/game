#pragma once
// Read-only memory mapped view of the standalone asset cooker format. This header is intentionally
// not included by the engine yet. TBDM v1 is little-endian, with 48-byte Vertex records.
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>

namespace tbd {
namespace cooked {
constexpr uint32_t kVersion = 1;
#pragma pack(push, 1)
struct Header {
    char magic[4]; uint32_t version, headerBytes, fileBytes;
    uint32_t vertexCount, indexCount, partCount, textureCount;
    uint32_t vertexOffset, indexOffset, partOffset, textureOffset;
    float bounds[6]; uint8_t sourceHash[32];
};
struct Part { uint32_t first, count, flags; uint16_t reserved0, reserved1; char name[64]; };
struct Texture { uint8_t format, type, flags, reserved; uint32_t width, height, mipCount, dataOffset; };
#pragma pack(pop)
static_assert(sizeof(Header) == 92, "TBDM header layout");
static_assert(sizeof(Part) == 80, "TBDM part layout");
static_assert(sizeof(Texture) == 20, "TBDM texture layout");

class Mapping {
public:
    Mapping() = default;
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    ~Mapping() { Close(); }
    void Close() {
        if (view_) { UnmapViewOfFile(view_); view_ = nullptr; }
        if (mapping_) { CloseHandle(mapping_); mapping_ = nullptr; }
        if (file_ != INVALID_HANDLE_VALUE) { CloseHandle(file_); file_ = INVALID_HANDLE_VALUE; }
        size_ = 0;
    }
    bool Open(const wchar_t* path, std::string* error = nullptr) {
        Close();
        file_ = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE) return Fail("cannot open cooked asset", error);
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file_, &length) || length.QuadPart < (LONGLONG)sizeof(Header) || length.QuadPart > UINT32_MAX)
            return Fail("invalid cooked asset size", error);
        size_ = (uint32_t)length.QuadPart;
        mapping_ = CreateFileMappingW(file_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_) return Fail("cannot create file mapping", error);
        view_ = (const uint8_t*)MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0);
        if (!view_) return Fail("cannot map cooked asset", error);
        if (!Validate(error)) { Close(); return false; }
        return true;
    }
    const Header* GetHeader() const { return (const Header*)view_; }
    const uint8_t* Vertices() const { return view_ + GetHeader()->vertexOffset; }
    const uint32_t* Indices() const { return (const uint32_t*)(view_ + GetHeader()->indexOffset); }
    const Part* Parts() const { return (const Part*)(view_ + GetHeader()->partOffset); }
    const Texture* Textures() const { return (const Texture*)(view_ + GetHeader()->textureOffset); }
    const uint8_t* BytesAt(uint32_t offset) const { return offset < size_ ? view_ + offset : nullptr; }
    uint32_t Size() const { return size_; }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE, mapping_ = nullptr;
    const uint8_t* view_ = nullptr; uint32_t size_ = 0;
    bool Fail(const char* why, std::string* error) { if (error) *error = why; Close(); return false; }
    bool Range(uint32_t offset, uint64_t bytes) const { return offset <= size_ && bytes <= (uint64_t)size_ - offset; }
    bool Validate(std::string* error) const {
        if (!view_) return Fail("asset is not mapped", error);
        const Header& h = *GetHeader();
        if (std::memcmp(h.magic, "TBDM", 4) || h.version != kVersion || h.headerBytes != sizeof(Header) || h.fileBytes != size_)
            return Fail("unsupported cooked asset header or version", error);
        if (!Range(h.vertexOffset, (uint64_t)h.vertexCount * 48) || !Range(h.indexOffset, (uint64_t)h.indexCount * 4) ||
            !Range(h.partOffset, (uint64_t)h.partCount * sizeof(Part)) || !Range(h.textureOffset, (uint64_t)h.textureCount * sizeof(Texture)))
            return Fail("cooked asset section outside file", error);
        const uint32_t* ix = Indices();
        for (uint32_t i = 0; i < h.indexCount; ++i) if (ix[i] >= h.vertexCount) return Fail("cooked index out of range", error);
        const Part* p = Parts();
        for (uint32_t i = 0; i < h.partCount; ++i) if ((uint64_t)p[i].first + p[i].count > h.indexCount) return Fail("cooked part out of range", error);
        const Texture* t = Textures();
        for (uint32_t i = 0; i < h.textureCount; ++i) {
            uint64_t bytes = 0; uint32_t w = t[i].width, height = t[i].height;
            for (uint32_t mip = 0; mip < t[i].mipCount; ++mip) { bytes += (uint64_t)w * height * 4; w = w > 1 ? w / 2 : 1; height = height > 1 ? height / 2 : 1; }
            if (t[i].format != 0 || !t[i].width || !t[i].height || !t[i].mipCount || !Range(t[i].dataOffset, bytes))
                return Fail("invalid cooked RGBA8 texture", error);
        }
        return true;
    }
};
} // namespace cooked
} // namespace tbd
