// The city sim's save file, `.tbdsim` (CS2 sim roadmap 3.4, Phase 0; frozen interface). Header only, headless
// (tests\sim_save_test). Binary, little-endian, beside the `.tbdgame` and `.hfd` of a slot:
//   "TBDSIM\0\0"  u32 formatVersion (1)  u32 chunkCount
//   chunk: u32 fourcc  u16 version  u16 flags  u32 bytes  u32 crc32 (of the payload)  payload
// Each system registers one chunk (fourcc, current version, save, load, fresh) in a SimSaveRegistry. On load:
//   - a chunk with a fourcc nobody registered is skipped (an older build reading a newer file);
//   - a chunk whose version is newer than the registered one, whose CRC is bad, that is missing, or whose Load fails or
//     reads past its payload makes THAT system start fresh (Fresh()); the others load normally;
//   - a bad magic, a newer formatVersion or a truncated header means no chunk is trusted: every system starts fresh.
// The caller logs the per-chunk report (SimLoadReport). Systems write their SoA columns raw with PutVec (count + bytes).
// Files go through tbd::platform::FsPath and std::filesystem::path streams (no wide-string stream constructors), written
// to "<file>.tmp" and renamed over the old one, so a crash mid-write never leaves half a save.
#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

#include "../platform/platform.h"
#include "sim_buildings.h"
#include "sim_ids.h"

namespace tbd {
namespace city {

static_assert(std::endian::native == std::endian::little, ".tbdsim is little-endian and written raw: a big-endian host needs byte swaps");

constexpr char kSimMagic[8] = {'T', 'B', 'D', 'S', 'I', 'M', '\0', '\0'};
constexpr uint32_t kSimFormatVersion = 1;
constexpr size_t kSimHeaderBytes = 16, kSimChunkHeaderBytes = 16;

// 'HEAD' as a u32 whose bytes on disk read H, E, A, D.
constexpr uint32_t FourCC(const char (&s)[5]) {
    return (uint32_t)(uint8_t)s[0] | (uint32_t)(uint8_t)s[1] << 8 | (uint32_t)(uint8_t)s[2] << 16 | (uint32_t)(uint8_t)s[3] << 24;
}
inline std::string FourCCName(uint32_t f) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; ++i) { const char c = (char)(f >> (8 * i)); s[(size_t)i] = c >= 32 && c < 127 ? c : '?'; }
    return s;
}

// CRC-32 (IEEE 802.3, reflected 0xEDB88320, the zlib / PNG one). Crc32("123456789") == 0xCBF43926.
inline uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1u ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}
inline uint32_t Crc32(const std::vector<uint8_t>& v) { return Crc32(v.data(), v.size()); }

// Appends little-endian values to a byte vector.
class ByteWriter {
public:
    std::vector<uint8_t> bytes;

    void Raw(const void* p, size_t n) { const uint8_t* b = (const uint8_t*)p; bytes.insert(bytes.end(), b, b + n); }
    template <typename T>
    void Put(T v) {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "Put writes numbers; use PutVec / Raw for structs");
        Raw(&v, sizeof(T));
    }
    void U8(uint8_t v) { Put(v); }
    void U16(uint16_t v) { Put(v); }
    void U32(uint32_t v) { Put(v); }
    void U64(uint64_t v) { Put(v); }
    void I32(int32_t v) { Put(v); }
    void I64(int64_t v) { Put(v); }
    void F32(float v) { Put(v); }
    void F64(double v) { Put(v); }
    void Str(const std::string& s) { U32((uint32_t)s.size()), Raw(s.data(), s.size()); }
    // A column written raw: u32 count, then count * sizeof(T) bytes. T must be trivially copyable; a struct with padding
    // writes its padding too (zero-initialise it, or the file differs run to run while still loading fine).
    template <typename T>
    void PutVec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>, "PutVec writes raw bytes");
        U32((uint32_t)v.size());
        if (!v.empty()) Raw(v.data(), v.size() * sizeof(T));
    }
    size_t Size() const { return bytes.size(); }
};

// Reads little-endian values back. Never reads past its span: an overrun sets ok = false and yields zeros, so a loader
// can read everything and check ok once at the end.
class ByteReader {
public:
    ByteReader() = default;
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}
    bool ok = true;

    bool Raw(void* out, size_t n) {
        if (!ok || n > n_ - pos_) { ok = false, std::memset(out, 0, n); return false; }
        std::memcpy(out, p_ + pos_, n);
        pos_ += n;
        return true;
    }
    template <typename T>
    T Get() {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "Get reads numbers; use GetVec / Raw for structs");
        T v{};
        Raw(&v, sizeof(T));
        return v;
    }
    uint8_t U8() { return Get<uint8_t>(); }
    uint16_t U16() { return Get<uint16_t>(); }
    uint32_t U32() { return Get<uint32_t>(); }
    uint64_t U64() { return Get<uint64_t>(); }
    int32_t I32() { return Get<int32_t>(); }
    int64_t I64() { return Get<int64_t>(); }
    float F32() { return Get<float>(); }
    double F64() { return Get<double>(); }
    std::string Str() {
        const uint32_t n = U32();
        if (!ok || n > Remaining()) { ok = false; return std::string(); }
        std::string s((const char*)p_ + pos_, n);
        pos_ += n;
        return s;
    }
    // The counterpart of ByteWriter::PutVec. A count larger than what is left fails (no huge allocation from a bad file).
    template <typename T>
    bool GetVec(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>, "GetVec reads raw bytes");
        const uint32_t n = U32();
        if (!ok || (uint64_t)n * sizeof(T) > Remaining()) { ok = false, v.clear(); return false; }
        v.resize(n);
        if (n) Raw(v.data(), (size_t)n * sizeof(T));
        return ok;
    }
    size_t Remaining() const { return n_ - pos_; }
    size_t Pos() const { return pos_; }
    bool AtEnd() const { return pos_ == n_; }

private:
    const uint8_t* p_ = nullptr;
    size_t n_ = 0, pos_ = 0;
};

// A parsed file: the header and a view of every chunk in file order (payload offsets into the file's bytes).
struct SimChunkView {
    uint32_t fourcc = 0;
    uint16_t version = 0, flags = 0;
    uint32_t offset = 0, bytes = 0, crc = 0;
    bool crcOk = false;
};
struct SimFileView {
    bool headerOk = false;       // magic, format version and chunk headers all sane
    uint32_t formatVersion = 0;
    uint32_t chunkCount = 0;     // as the header says
    bool truncated = false;      // the file ended inside a chunk; the chunks before it are kept
    std::vector<SimChunkView> chunks;
    const SimChunkView* Find(uint32_t fourcc) const {
        for (const SimChunkView& c : chunks) if (c.fourcc == fourcc) return &c; // the first wins if one repeats
        return nullptr;
    }
};

inline SimFileView ParseSimFile(const std::vector<uint8_t>& file) {
    SimFileView v;
    if (file.size() < kSimHeaderBytes || std::memcmp(file.data(), kSimMagic, 8) != 0) return v;
    ByteReader r(file.data() + 8, file.size() - 8);
    v.formatVersion = r.U32();
    v.chunkCount = r.U32();
    if (v.formatVersion == 0 || v.formatVersion > kSimFormatVersion) return v; // a newer layout: trust nothing
    v.headerOk = true;
    size_t pos = kSimHeaderBytes;
    for (uint32_t i = 0; i < v.chunkCount; ++i) {
        if (file.size() - pos < kSimChunkHeaderBytes) { v.truncated = true; break; }
        ByteReader h(file.data() + pos, kSimChunkHeaderBytes);
        SimChunkView c;
        c.fourcc = h.U32(), c.version = h.U16(), c.flags = h.U16(), c.bytes = h.U32(), c.crc = h.U32();
        pos += kSimChunkHeaderBytes;
        if (c.bytes > file.size() - pos) { v.truncated = true; break; }
        c.offset = (uint32_t)pos;
        c.crcOk = Crc32(file.data() + pos, c.bytes) == c.crc;
        v.chunks.push_back(c);
        pos += c.bytes;
    }
    return v;
}

// What happened to each registered chunk on load.
enum class SimChunkStatus : uint8_t { Loaded, Missing, Newer, BadCrc, Corrupt, BadFile };
inline const char* SimChunkStatusName(SimChunkStatus s) {
    switch (s) {
    case SimChunkStatus::Loaded: return "loaded";
    case SimChunkStatus::Missing: return "missing (fresh)";
    case SimChunkStatus::Newer: return "newer version than this build (fresh)";
    case SimChunkStatus::BadCrc: return "bad CRC (fresh)";
    case SimChunkStatus::Corrupt: return "corrupt payload (fresh)";
    case SimChunkStatus::BadFile: return "unreadable file (fresh)";
    }
    return "?";
}
struct SimChunkResult {
    uint32_t fourcc = 0;
    uint16_t fileVersion = 0; // the chunk's version in the file (0 if missing)
    SimChunkStatus status = SimChunkStatus::Missing;
};
struct SimLoadReport {
    bool fileOk = false;                   // header trusted
    bool truncated = false;
    std::vector<SimChunkResult> chunks;    // one per registered chunk, in registration order
    std::vector<uint32_t> skipped;         // fourccs in the file that nobody registered
    int Loaded() const { int n = 0; for (const SimChunkResult& c : chunks) n += c.status == SimChunkStatus::Loaded; return n; }
    const SimChunkResult* Find(uint32_t f) const { for (const SimChunkResult& c : chunks) if (c.fourcc == f) return &c; return nullptr; }
};

// The chunk registry: every system's (fourcc, version, save, load, fresh). Systems register once at init; the order of
// registration is the order in the file. Load gets the file's chunk version (<= the registered one) and must consume
// what it understands; returning false (or reading past the payload) means corrupt -> Fresh.
struct SimChunkDef {
    uint32_t fourcc = 0;
    uint16_t version = 1;
    uint16_t flags = 0;
    std::function<void(ByteWriter&)> save;
    std::function<bool(ByteReader&, uint16_t version)> load;
    std::function<void()> fresh;
};

class SimSaveRegistry {
public:
    // False if the fourcc is already registered (each phase owns its ids: roadmap 3.4 table).
    bool Register(SimChunkDef def) {
        if (Find(def.fourcc)) return false;
        defs_.push_back(std::move(def));
        return true;
    }
    const SimChunkDef* Find(uint32_t fourcc) const { for (const SimChunkDef& d : defs_) if (d.fourcc == fourcc) return &d; return nullptr; }
    const std::vector<SimChunkDef>& Defs() const { return defs_; }
    void Clear() { defs_.clear(); }

    std::vector<uint8_t> Save() const { return Assemble(Snapshot()); }

    // Save in two steps, for the autosave: Snapshot runs every chunk's save (the main thread: it reads live state) and
    // keeps the payloads; Assemble (any thread) adds the header, the chunk headers and the CRCs. Same bytes as Save().
    struct ChunkImage {
        uint32_t fourcc = 0;
        uint16_t version = 1, flags = 0;
        std::vector<uint8_t> payload;
    };
    std::vector<ChunkImage> Snapshot() const {
        std::vector<ChunkImage> out;
        out.reserve(defs_.size());
        for (const SimChunkDef& d : defs_) {
            ByteWriter p;
            if (d.save) d.save(p);
            out.push_back({d.fourcc, d.version, d.flags, std::move(p.bytes)});
        }
        return out;
    }
    static std::vector<uint8_t> Assemble(const std::vector<ChunkImage>& chunks) {
        size_t n = kSimHeaderBytes;
        for (const ChunkImage& c : chunks) n += kSimChunkHeaderBytes + c.payload.size();
        ByteWriter w;
        w.bytes.reserve(n);
        w.Raw(kSimMagic, 8);
        w.U32(kSimFormatVersion);
        w.U32((uint32_t)chunks.size());
        for (const ChunkImage& c : chunks) {
            w.U32(c.fourcc), w.U16(c.version), w.U16(c.flags), w.U32((uint32_t)c.payload.size()), w.U32(Crc32(c.payload));
            w.Raw(c.payload.data(), c.payload.size());
        }
        return std::move(w.bytes);
    }

    // Loads every registered chunk it can; each one that cannot be trusted starts fresh. Never throws.
    SimLoadReport Load(const std::vector<uint8_t>& file) const {
        SimLoadReport rep;
        const SimFileView v = ParseSimFile(file);
        rep.fileOk = v.headerOk, rep.truncated = v.truncated;
        for (const SimChunkView& c : v.chunks)
            if (!Find(c.fourcc)) rep.skipped.push_back(c.fourcc);
        for (const SimChunkDef& d : defs_) {
            SimChunkResult res;
            res.fourcc = d.fourcc;
            const SimChunkView* c = v.headerOk ? v.Find(d.fourcc) : nullptr;
            if (!v.headerOk) res.status = file.empty() ? SimChunkStatus::Missing : SimChunkStatus::BadFile;
            else if (!c) res.status = SimChunkStatus::Missing;
            else if (res.fileVersion = c->version, c->version > d.version) res.status = SimChunkStatus::Newer;
            else if (!c->crcOk) res.status = SimChunkStatus::BadCrc;
            else {
                ByteReader r(file.data() + c->offset, c->bytes);
                const bool ok = d.load ? d.load(r, c->version) : true;
                res.status = ok && r.ok ? SimChunkStatus::Loaded : SimChunkStatus::Corrupt;
            }
            if (res.status != SimChunkStatus::Loaded && d.fresh) d.fresh();
            rep.chunks.push_back(res);
        }
        return rep;
    }

private:
    std::vector<SimChunkDef> defs_;
};

// The chunk ids of roadmap 3.4, by owning phase. A phase registers only its own; a new id goes through a Phase 0 PR.
namespace simchunk {
constexpr uint32_t HEAD = FourCC("HEAD"), BLDG = FourCC("BLDG");                                                  // P0
constexpr uint32_t CITZ = FourCC("CITZ"), HHLD = FourCC("HHLD"), OCCU = FourCC("OCCU"), MIGR = FourCC("MIGR");   // P1
constexpr uint32_t GROW = FourCC("GROW"), LAND = FourCC("LAND"), DMND = FourCC("DMND");                          // P2
constexpr uint32_t POLL = FourCC("POLL"), WTHR = FourCC("WTHR"), WATR = FourCC("WATR");                          // P3
constexpr uint32_t SERV = FourCC("SERV"), INCD = FourCC("INCD"), GARB = FourCC("GARB");                          // P4
constexpr uint32_t LINE = FourCC("LINE"), MODE = FourCC("MODE"), OUTC = FourCC("OUTC");                          // P5
constexpr uint32_t COMP = FourCC("COMP"), GOOD = FourCC("GOOD"), TRAD = FourCC("TRAD");                          // P6
constexpr uint32_t BUDG = FourCC("BUDG"), PLCY = FourCC("PLCY"), DIST = FourCC("DIST"), MILE = FourCC("MILE");   // P7
} // namespace simchunk

// HEAD (P0): the sim clock and seeds.
struct SimSaveHead {
    int64_t minute = 0;
    int64_t lifeMonth = 0;
    uint64_t citySeed = 0;
    uint32_t recordCap = 0;
    uint32_t autosaveCounter = 0;
};
inline void WriteHead(ByteWriter& w, const SimSaveHead& h) { w.I64(h.minute), w.I64(h.lifeMonth), w.U64(h.citySeed), w.U32(h.recordCap), w.U32(h.autosaveCounter); }
inline bool ReadHead(ByteReader& r, SimSaveHead& h) {
    h.minute = r.I64(), h.lifeMonth = r.I64(), h.citySeed = r.U64(), h.recordCap = r.U32(), h.autosaveCounter = r.U32();
    return r.ok;
}

// BLDG (P0): the registry's handle pool and each slot's (origin, originId) plus the row data, field by field (no padding
// on disk). Load puts the same BuildingIds back as reserved rows; the next syncs claim them by key.
inline void WriteBuildings(ByteWriter& w, const BuildingRegistry& reg) {
    const HandlePool& p = reg.Pool();
    w.bytes.reserve(w.bytes.size() + 16 + (size_t)reg.Capacity() * (2 + 48)); // one allocation (the autosave copy runs on the main thread)
    w.PutVec(p.Gens()), w.PutVec(p.AliveBits()), w.PutVec(p.FreeList());
    w.U32(reg.Capacity()); // one row per pool slot (an empty registry has rows.size() 0 but slot 0 reserved)
    // Each row packed field by field (no padding on disk) into one buffer, then appended once: the same bytes as
    // U8 origin, U64 originId, I32 kind cellX cellZ cell hood entranceNode, U8 level, U16 homes, U16 jobSlots[5].
    constexpr size_t kRowBytes = 1 + 8 + 6 * 4 + 1 + 2 + 2 * kEduLevels;
    const BuildingRow empty;
    for (uint32_t i = 0; i < reg.Capacity(); ++i) {
        const BuildingRow& r = i < reg.rows.size() ? reg.rows[i] : empty;
        uint8_t b[kRowBytes];
        uint8_t* q = b;
        auto put = [&q](const void* v, size_t n) { std::memcpy(q, v, n), q += n; };
        const uint8_t origin = (uint8_t)r.origin;
        put(&origin, 1), put(&r.originId, 8), put(&r.kind, 4), put(&r.cellX, 4), put(&r.cellZ, 4), put(&r.cell, 4), put(&r.hood, 4);
        put(&r.entranceNode, 4), put(&r.level, 1), put(&r.homes, 2), put(r.jobSlots, 2 * kEduLevels);
        w.Raw(b, kRowBytes);
    }
}
inline bool ReadBuildings(ByteReader& r, BuildingRegistry& reg) {
    std::vector<uint8_t> gens, alive;
    std::vector<uint32_t> freeList;
    r.GetVec(gens), r.GetVec(alive), r.GetVec(freeList);
    const uint32_t n = r.U32();
    if (!r.ok || n != gens.size()) return false;
    std::vector<BuildingRow> rows(n);
    for (BuildingRow& b : rows) {
        b.origin = (BOrigin)r.U8(), b.originId = r.U64(), b.kind = r.I32(), b.cellX = r.I32(), b.cellZ = r.I32(), b.cell = r.I32();
        b.hood = r.I32(), b.entranceNode = r.I32(), b.level = r.U8(), b.homes = r.U16();
        for (int e = 0; e < kEduLevels; ++e) b.jobSlots[e] = r.U16();
    }
    if (!r.ok) return false;
    HandlePool pool;
    if (!pool.SetRaw(std::move(gens), std::move(alive), std::move(freeList))) return false;
    return reg.Restore(pool, rows);
}

// Files. `path` is a game path (Windows separators); FsPath maps it for the OS.
inline bool ReadSimFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    std::ifstream f(tbd::platform::FsPath(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0, std::ios::beg);
    out.resize((size_t)n);
    if (n > 0 && !f.read((char*)out.data(), n)) { out.clear(); return false; }
    return true;
}
// Writes "<path>.tmp", then renames it over `path`.
inline bool WriteSimFileBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    const std::filesystem::path tmp = tbd::platform::FsPath(path + L".tmp"), dst = tbd::platform::FsPath(path);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (!bytes.empty()) f.write((const char*)bytes.data(), (std::streamsize)bytes.size());
        f.flush();
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, dst, ec); // replaces an existing file
    if (ec) { std::filesystem::remove(tmp, ec); return false; }
    return true;
}

} // namespace city
} // namespace tbd
