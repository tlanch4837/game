// Headless checks for the city sim's save file (src/city/sim_save.h) and the per-phase tuning (sim_tuning_table.h):
//   CRC-32 known value; ByteWriter / ByteReader round trip and overrun; a multi-chunk round trip through the registry and
//   a real file; an unknown chunk is skipped; a newer chunk version, a bad CRC, a corrupt payload or a missing chunk make
//   only that system start fresh; a bad magic, a newer format version or a truncated file are handled; BuildingRegistry
//   ids stay stable across a lots compaction and survive the BLDG chunk; prefixed tuning names go to their own table only.
// Build and run: tests\sim_save_test.bat
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "city/sim_save.h"
#include "city/sim_tuning.h"
#include "city/sim_tuning_table.h"

using namespace tbd::city;

static int failures = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } \
    } while (0)

// Two toy systems standing in for phases: one with a scalar and a column, one with a string.
struct SysA {
    int64_t minute = 0;
    std::vector<uint16_t> col;
    int freshCalls = 0;
    void Fresh() { minute = -1, col.clear(), ++freshCalls; }
};
struct SysB {
    std::string name;
    double money = 0;
    int freshCalls = 0;
};

static void RegisterAB(SimSaveRegistry& reg, SysA& a, SysB& b, uint16_t verA = 1, uint16_t verB = 2) {
    SimChunkDef da;
    da.fourcc = FourCC("TSTA"), da.version = verA;
    da.save = [&a](ByteWriter& w) { w.I64(a.minute), w.PutVec(a.col); };
    da.load = [&a](ByteReader& r, uint16_t) { a.minute = r.I64(); return r.GetVec(a.col) && r.AtEnd(); };
    da.fresh = [&a] { a.Fresh(); };
    CHECK(reg.Register(da));
    SimChunkDef db;
    db.fourcc = FourCC("TSTB"), db.version = verB;
    db.save = [&b](ByteWriter& w) { w.Str(b.name), w.F64(b.money); };
    db.load = [&b](ByteReader& r, uint16_t v) { b.name = r.Str(); b.money = v >= 2 ? r.F64() : 0.0; return r.ok; };
    db.fresh = [&b] { b.name.clear(), b.money = 0, ++b.freshCalls; };
    CHECK(reg.Register(db));
    CHECK(!reg.Register(db)); // a fourcc registers once
}

static void TestCrcAndBytes() {
    const char* s = "123456789";
    CHECK(Crc32((const uint8_t*)s, 9) == 0xCBF43926u);
    CHECK(Crc32(std::vector<uint8_t>{}) == 0u);
    CHECK(FourCC("HEAD") == 0x44414548u);
    CHECK(FourCCName(simchunk::BLDG) == "BLDG");

    ByteWriter w;
    w.U8(7), w.U16(0xBEEF), w.U32(0xDEADBEEFu), w.U64(0x0123456789ABCDEFull), w.I32(-5), w.I64(-6), w.F32(1.5f), w.F64(-2.25);
    w.Str("tbd");
    w.PutVec(std::vector<int32_t>{1, -2, 3});
    CHECK(w.bytes[1] == 0xEF && w.bytes[2] == 0xBE); // little-endian
    ByteReader r(w.bytes);
    CHECK(r.U8() == 7 && r.U16() == 0xBEEF && r.U32() == 0xDEADBEEFu && r.U64() == 0x0123456789ABCDEFull);
    CHECK(r.I32() == -5 && r.I64() == -6 && r.F32() == 1.5f && r.F64() == -2.25);
    CHECK(r.Str() == "tbd");
    std::vector<int32_t> v;
    CHECK(r.GetVec(v) && v.size() == 3 && v[1] == -2);
    CHECK(r.ok && r.AtEnd());
    CHECK(r.U32() == 0 && !r.ok); // overrun: zero, not ok
    // A column count larger than the bytes left fails without allocating.
    ByteWriter w2;
    w2.U32(0x7FFFFFFFu);
    ByteReader r2(w2.bytes);
    std::vector<uint64_t> big;
    CHECK(!r2.GetVec(big) && big.empty() && !r2.ok);
}

static std::vector<uint8_t> SaveSample(uint16_t verA = 1, bool extra = false) {
    SysA a;
    SysB b;
    a.minute = 123456789012ll, a.col = {1, 2, 3, 65535};
    b.name = "Tbd City", b.money = 1500000.25;
    SimSaveRegistry reg;
    if (extra) { // a chunk this build will not know, written first
        SimChunkDef dx;
        dx.fourcc = FourCC("XTRA"), dx.version = 9;
        dx.save = [](ByteWriter& w) { w.Str("from the future"), w.U64(42); };
        CHECK(reg.Register(dx));
    }
    RegisterAB(reg, a, b, verA);
    return reg.Save();
}

static void TestRoundTrip() {
    const std::vector<uint8_t> file = SaveSample();
    const SimFileView v = ParseSimFile(file);
    CHECK(v.headerOk && v.formatVersion == kSimFormatVersion && v.chunkCount == 2 && v.chunks.size() == 2 && !v.truncated);
    CHECK(v.chunks[0].fourcc == FourCC("TSTA") && v.chunks[1].version == 2 && v.chunks[0].crcOk && v.chunks[1].crcOk);
    CHECK(std::memcmp(file.data(), "TBDSIM\0\0", 8) == 0);

    SysA a;
    SysB b;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.fileOk && rep.Loaded() == 2 && rep.skipped.empty());
    CHECK(a.minute == 123456789012ll && a.col.size() == 4 && a.col[3] == 65535 && a.freshCalls == 0);
    CHECK(b.name == "Tbd City" && b.money == 1500000.25 && b.freshCalls == 0);
    CHECK(reg.Save() == file); // byte-identical re-save

    // Through a real file (tmp + rename), twice so the rename replaces an existing file.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "tbd_sim_save_test";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::wstring path = (dir / "slot0.tbdsim").wstring();
    CHECK(WriteSimFileBytes(path, std::vector<uint8_t>{1, 2, 3}));
    CHECK(WriteSimFileBytes(path, file));
    std::vector<uint8_t> back;
    CHECK(ReadSimFileBytes(path, back) && back == file);
    CHECK(!std::filesystem::exists(dir / "slot0.tbdsim.tmp"));
    CHECK(!ReadSimFileBytes((dir / "missing.tbdsim").wstring(), back) && back.empty());
    std::filesystem::remove_all(dir, ec);
}

static void TestUnknownSkipped() {
    const std::vector<uint8_t> file = SaveSample(1, true);
    SysA a;
    SysB b;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.fileOk && rep.Loaded() == 2);
    CHECK(rep.skipped.size() == 1 && rep.skipped[0] == FourCC("XTRA"));
    CHECK(a.col.size() == 4 && b.name == "Tbd City");
}

static void TestNewerVersionFresh() {
    const std::vector<uint8_t> file = SaveSample(3); // TSTA saved at version 3, this build knows 1
    SysA a;
    SysB b;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.Find(FourCC("TSTA"))->status == SimChunkStatus::Newer && rep.Find(FourCC("TSTA"))->fileVersion == 3);
    CHECK(a.freshCalls == 1 && a.minute == -1 && a.col.empty());
    CHECK(rep.Find(FourCC("TSTB"))->status == SimChunkStatus::Loaded && b.name == "Tbd City"); // the other system loads
}

static void TestOlderVersionLoads() {
    // TSTB saved by an older build at version 1 (no money field): loads with the version it was written at.
    std::vector<uint8_t> file;
    {
        SimSaveRegistry r1; // version 1 TSTB writes only the name
        SimChunkDef db;
        db.fourcc = FourCC("TSTB"), db.version = 1;
        db.save = [](ByteWriter& w) { w.Str("Old"); };
        CHECK(r1.Register(db));
        file = r1.Save();
    }
    SysA a;
    SysB b;
    b.money = 7;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.Find(FourCC("TSTB"))->status == SimChunkStatus::Loaded && b.name == "Old" && b.money == 0.0);
    CHECK(rep.Find(FourCC("TSTA"))->status == SimChunkStatus::Missing && a.freshCalls == 1);
}

static void TestBadCrcFresh() {
    std::vector<uint8_t> file = SaveSample();
    const SimFileView v = ParseSimFile(file);
    file[v.chunks[1].offset + 5] ^= 0x40; // a flipped bit inside TSTB's name
    SysA a;
    SysB b;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.Find(FourCC("TSTB"))->status == SimChunkStatus::BadCrc && b.freshCalls == 1 && b.name.empty());
    CHECK(rep.Find(FourCC("TSTA"))->status == SimChunkStatus::Loaded && a.col.size() == 4);
}

static void TestCorruptPayloadFresh() {
    // A chunk with a good CRC whose payload is short for its loader (it reads past the end): Corrupt, fresh.
    SimSaveRegistry w;
    SimChunkDef d;
    d.fourcc = FourCC("TSTA"), d.version = 1;
    d.save = [](ByteWriter& bw) { bw.U32(5); }; // 4 bytes; TSTA's loader wants an i64 and a column
    CHECK(w.Register(d));
    const std::vector<uint8_t> file = w.Save();
    SysA a;
    SysB b;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    const SimLoadReport rep = reg.Load(file);
    CHECK(rep.Find(FourCC("TSTA"))->status == SimChunkStatus::Corrupt && a.freshCalls == 1);
    CHECK(rep.Find(FourCC("TSTB"))->status == SimChunkStatus::Missing && b.freshCalls == 1);
}

static void TestBadFiles() {
    const std::vector<uint8_t> good = SaveSample();
    auto loadAll = [](const std::vector<uint8_t>& f, int& freshA, int& freshB) {
        SysA a;
        SysB b;
        SimSaveRegistry reg;
        RegisterAB(reg, a, b);
        const SimLoadReport rep = reg.Load(f);
        freshA = a.freshCalls, freshB = b.freshCalls;
        return rep;
    };
    int fa = 0, fb = 0;
    // Empty (no .tbdsim yet): every system fresh, status Missing.
    SimLoadReport rep = loadAll({}, fa, fb);
    CHECK(!rep.fileOk && fa == 1 && fb == 1 && rep.chunks[0].status == SimChunkStatus::Missing);
    // Bad magic.
    std::vector<uint8_t> f = good;
    f[0] = 'X';
    rep = loadAll(f, fa, fb);
    CHECK(!rep.fileOk && fa == 1 && fb == 1 && rep.chunks[1].status == SimChunkStatus::BadFile);
    // A newer format version: nothing trusted.
    f = good;
    f[8] = (uint8_t)(kSimFormatVersion + 1);
    rep = loadAll(f, fa, fb);
    CHECK(!rep.fileOk && fa == 1 && fb == 1 && rep.Loaded() == 0);
    // Truncated inside the second chunk: the first still loads, the second is fresh.
    f = good;
    f.resize(f.size() - 3);
    rep = loadAll(f, fa, fb);
    CHECK(rep.fileOk && rep.truncated && fa == 0 && fb == 1);
    CHECK(rep.Find(FourCC("TSTA"))->status == SimChunkStatus::Loaded && rep.Find(FourCC("TSTB"))->status == SimChunkStatus::Missing);
    // Truncated inside the 16-byte header: the format is unreadable.
    f.resize(10);
    rep = loadAll(f, fa, fb);
    CHECK(!rep.fileOk && fa == 1 && fb == 1);
    // A huge chunk byte count does not read past the file.
    f = good;
    f[kSimHeaderBytes + 8] = 0xFF, f[kSimHeaderBytes + 9] = 0xFF, f[kSimHeaderBytes + 10] = 0xFF, f[kSimHeaderBytes + 11] = 0x7F;
    rep = loadAll(f, fa, fb);
    CHECK(rep.fileOk && rep.truncated && fa == 1 && fb == 1);
}

static void TestHead() {
    SimSaveHead h;
    h.minute = 1440ll * 400 + 7, h.lifeMonth = 400, h.citySeed = 0xC0FFEEull, h.recordCap = 50000, h.autosaveCounter = 3;
    ByteWriter w;
    WriteHead(w, h);
    ByteReader r(w.bytes);
    SimSaveHead g;
    CHECK(ReadHead(r, g) && r.AtEnd());
    CHECK(g.minute == h.minute && g.lifeMonth == 400 && g.citySeed == 0xC0FFEEull && g.recordCap == 50000 && g.autosaveCounter == 3);
    ByteReader shortR(w.bytes.data(), 10);
    CHECK(!ReadHead(shortR, g));
}

// The host's lots vector, compacted (erase shifts positions) the way city zoning does; ids key on Lot::id, not position.
struct FakeLot { uint64_t id; int zone; uint16_t homes; };
static void Sync(BuildingRegistry& reg, const std::vector<FakeLot>& lots, std::vector<BuildingId>* out = nullptr) {
    reg.BeginSync();
    if (out) out->clear();
    for (const FakeLot& l : lots) {
        BuildingRow row;
        row.kind = l.zone, row.homes = l.homes, row.cellX = (int32_t)(l.id % 7), row.cellZ = -(int32_t)(l.id % 5);
        const BuildingId b = reg.Upsert(BOrigin::Lot, l.id, row);
        if (out) out->push_back(b);
    }
    reg.EndSync();
}

static void TestRegistryStable() {
    std::vector<FakeLot> lots;
    for (uint64_t i = 0; i < 10; ++i) lots.push_back({1000 + i * 3, (int)(i % 4), (uint16_t)(i * 2)});
    BuildingRegistry reg;
    std::vector<BuildingId> ids;
    Sync(reg, lots, &ids);
    CHECK(reg.Live() == 10);
    const BuildingId removed = ids[2], kept7 = ids[7];
    // Compaction: lots 2 and 5 demolished, the vector shifts down; the rest keep their ids.
    lots.erase(lots.begin() + 5), lots.erase(lots.begin() + 2);
    std::vector<BuildingId> after;
    Sync(reg, lots, &after);
    CHECK(reg.Live() == 8 && !reg.Alive(removed) && reg.Get(removed) == nullptr);
    CHECK(after[5] == kept7); // lot 7 now sits at position 5
    for (size_t i = 0; i < lots.size(); ++i) CHECK(reg.Find(BOrigin::Lot, lots[i].id) == after[i]);
    // A new lot reuses a freed slot with a new generation: the stale id still does not match.
    lots.push_back({5000, 1, 4});
    Sync(reg, lots, &after);
    const BuildingId fresh = after.back();
    CHECK(reg.Alive(fresh) && fresh != removed && reg.Live() == 9);
    CHECK(!reg.Alive(removed));
    // Same lots, same ids: a sync with no change does not bump the version.
    const uint64_t ver = reg.version;
    Sync(reg, lots);
    CHECK(reg.version == ver);

    // BLDG round trip: handles come back reserved, the next sync claims each by key, with the same id.
    ByteWriter w;
    WriteBuildings(w, reg);
    BuildingRegistry loaded;
    ByteReader r(w.bytes);
    CHECK(ReadBuildings(r, loaded) && r.AtEnd());
    CHECK(loaded.Live() == reg.Live() && loaded.Capacity() == reg.Capacity());
    for (const FakeLot& l : lots) CHECK(!loaded.Alive(loaded.Find(BOrigin::Lot, l.id)) && loaded.Find(BOrigin::Lot, l.id).Valid()); // reserved
    std::vector<FakeLot> back(lots.begin() + 1, lots.end()); // the first lot did not come back
    std::reverse(back.begin(), back.end());                    // and they come back in another order
    std::vector<BuildingId> claimed;
    Sync(loaded, back, &claimed);
    for (size_t i = 0; i < back.size(); ++i) CHECK(claimed[i] == reg.Find(BOrigin::Lot, back[i].id) && loaded.Alive(claimed[i]));
    const BuildingId lost = reg.Find(BOrigin::Lot, lots[0].id);
    CHECK(loaded.Get(lost) && loaded.Get(lost)->alive == 2); // still reserved until released
    loaded.ReleaseUnclaimed();
    CHECK(!loaded.Alive(lost) && loaded.Get(lost) == nullptr && loaded.Live() == back.size());
    // The row data came back too.
    const BuildingRow* row = loaded.Get(claimed[0]);
    CHECK(row && row->homes == back[0].homes && row->kind == back[0].zone && row->cellZ == -(int32_t)(back[0].id % 5));

    // An empty registry round-trips (slot 0 only).
    BuildingRegistry empty, empty2;
    ByteWriter we;
    WriteBuildings(we, empty);
    ByteReader re(we.bytes);
    CHECK(ReadBuildings(re, empty2) && empty2.Live() == 0);
    // A short BLDG payload fails.
    ByteReader rs(w.bytes.data(), w.bytes.size() / 2);
    BuildingRegistry bad;
    CHECK(!ReadBuildings(rs, bad));
}

// The hinted Upsert (one BuildingId per source entry, city_sim_save.inl) gives exactly the plain Upsert's registry through
// compactions, demolitions, new lots, a level change, a load and stale hints; CheckIndex agrees with the rows.
static void TestRegistryHints() {
    std::vector<FakeLot> lots;
    for (uint64_t i = 0; i < 40; ++i) lots.push_back({100 + i * 7, (int)(i % 5), (uint16_t)i});
    BuildingRegistry plain, hinted;
    std::vector<BuildingId> hints;
    auto both = [&](const std::vector<FakeLot>& ls) {
        Sync(plain, ls);
        hinted.BeginSync();
        if (hints.size() != ls.size()) hints.resize(ls.size());
        for (size_t i = 0; i < ls.size(); ++i) {
            BuildingRow row;
            row.kind = ls[i].zone, row.homes = ls[i].homes, row.cellX = (int32_t)(ls[i].id % 7), row.cellZ = -(int32_t)(ls[i].id % 5);
            hinted.Upsert(BOrigin::Lot, ls[i].id, row, hints[i]);
        }
        hinted.EndSync();
        bool same = plain.version == hinted.version && plain.Capacity() == hinted.Capacity() && plain.Live() == hinted.Live();
        for (uint32_t i = 1; same && i < plain.Capacity(); ++i) {
            const BuildingRow *a = plain.Get(plain.At(i)), *b = hinted.Get(hinted.At(i));
            same = (a == nullptr) == (b == nullptr) && plain.At(i) == hinted.At(i) && (!a || (a->originId == b->originId && a->homes == b->homes && a->kind == b->kind));
        }
        for (size_t i = 0; i < ls.size(); ++i) same = same && hints[i] == hinted.Find(BOrigin::Lot, ls[i].id);
        CHECK(same && hinted.CheckIndex() && plain.CheckIndex());
    };
    both(lots);
    both(lots);                                                  // unchanged: every hint used
    lots.erase(lots.begin() + 3), lots.erase(lots.begin() + 10); // compaction: the hints after 3 point at other lots
    both(lots);
    lots.push_back({9001, 2, 9}), lots[5].homes = 77; // a new lot and a changed one
    both(lots);
    std::swap(lots[0], lots[20]); // reordered
    both(lots);
    ByteWriter w; // a load: reserved rows claimed through stale hints
    WriteBuildings(w, hinted);
    ByteReader r(w.bytes);
    BuildingRegistry loaded;
    CHECK(ReadBuildings(r, loaded) && loaded.CheckIndex());
    hinted = loaded;
    ByteReader r2(w.bytes);
    plain = BuildingRegistry();
    CHECK(ReadBuildings(r2, plain));
    both(lots);
}

static void TestTuningTables() {
    const std::string json = R"({ "demandTauMinutes": 720, "life.instantFill": 1, "life.enabled": 0, "life.nope": 3,
        "env.enabled": 0.5, "gov.typo": 2, "lifeX.enabled": 9, "note": "life.enabled", "svc.enabled": -1 })";
    PhaseTuning t;
    std::vector<std::string> unknown;
    const int applied = t.ParseJson(json, &unknown);
    CHECK(applied == 4);
    CHECK(t.life.instantFill == 1.0 && t.life.enabled == 0.0 && t.env.enabled == 0.5 && t.svc.enabled == -1.0);
    CHECK(t.land.enabled == 1.0 && t.move.enabled == 1.0 && t.goods.enabled == 1.0 && t.gov.enabled == 1.0); // untouched
    CHECK(unknown.size() == 2 && unknown[0] == "life.nope" && unknown[1] == "gov.typo");
    // One table alone ignores every other prefix and the unprefixed set, silently.
    LifePhaseTuning life;
    std::vector<std::string> u2;
    CHECK(life.ParseJson(json, &u2) == 2 && u2.size() == 1);
    CHECK(!life.Set("env.enabled", 0) && !life.Set("instantFill", 0) && life.Set("life.instantFill", 2) && life.instantFill == 2.0);
    int n = 0;
    life.ForEach([&](const char* name, double v, double def) { ++n; if (std::string(name) == "instantFill") CHECK(v == 2.0 && def == 0.0); });
    CHECK(n == 2);
    // SimTuning reads the same text: it takes the unprefixed name and lists the prefixed ones as unknown; the host
    // filters those with IsPhaseTuningKey.
    SimTuning base;
    std::vector<std::string> u3;
    CHECK(base.ParseJson(json, &u3) == 1 && base.demandTauMinutes == 720.0);
    int unprefixedUnknown = 0;
    for (const std::string& k : u3) unprefixedUnknown += IsPhaseTuningKey(k) ? 0 : 1;
    CHECK(unprefixedUnknown == 1); // "lifeX.enabled": no phase owns it
    CHECK(IsPhaseTuningKey("goods.x") && !IsPhaseTuningKey("good.x") && !IsPhaseTuningKey("demandTauMinutes"));

    // From a file.
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "tbd_sim_tuning_test.json";
    {
        std::ofstream f(p, std::ios::binary);
        f << json;
    }
    PhaseTuning tf;
    int a = 0;
    CHECK(tf.LoadFile(p.wstring(), nullptr, &a) && a == 4 && tf.life.instantFill == 1.0);
    std::error_code ec;
    std::filesystem::remove(p, ec);
    CHECK(!tf.LoadFile(p.wstring(), nullptr, &a));
}

// The autosave's two steps (Snapshot on the main thread, Assemble on a worker) give Save()'s bytes, and BLDG's packed
// rows are the field-by-field layout.
static void TestSnapshotAssemble() {
    SysA a;
    SysB b;
    a.minute = 77, a.col = {9, 8, 7};
    b.name = "Snap", b.money = 3.5;
    SimSaveRegistry reg;
    RegisterAB(reg, a, b);
    BuildingRegistry br;
    br.BeginSync();
    BuildingRow row;
    row.kind = 3, row.cellX = -4, row.cellZ = 9, row.cell = 12, row.hood = 2, row.entranceNode = 40, row.level = 2, row.homes = 17;
    for (int e = 0; e < kEduLevels; ++e) row.jobSlots[e] = (uint16_t)(e * 3 + 1);
    br.Upsert(BOrigin::Lot, 0x1122334455ull, row);
    br.Upsert(BOrigin::Utility, 5, BuildingRow());
    br.EndSync();
    SimChunkDef d;
    d.fourcc = simchunk::BLDG;
    d.save = [&br](ByteWriter& w) { WriteBuildings(w, br); };
    d.load = [](ByteReader&, uint16_t) { return true; };
    CHECK(reg.Register(d));
    const std::vector<uint8_t> whole = reg.Save();
    CHECK(SimSaveRegistry::Assemble(reg.Snapshot()) == whole);
    // the packed rows against the field-by-field writer
    ByteWriter packed, fields;
    WriteBuildings(packed, br);
    const HandlePool& p = br.Pool();
    fields.PutVec(p.Gens()), fields.PutVec(p.AliveBits()), fields.PutVec(p.FreeList()), fields.U32(br.Capacity());
    for (uint32_t i = 0; i < br.Capacity(); ++i) {
        const BuildingRow r = i < br.rows.size() ? br.rows[i] : BuildingRow();
        fields.U8((uint8_t)r.origin), fields.U64(r.originId), fields.I32(r.kind), fields.I32(r.cellX), fields.I32(r.cellZ), fields.I32(r.cell);
        fields.I32(r.hood), fields.I32(r.entranceNode), fields.U8(r.level), fields.U16(r.homes);
        for (int e = 0; e < kEduLevels; ++e) fields.U16(r.jobSlots[e]);
    }
    CHECK(packed.bytes == fields.bytes);
    BuildingRegistry back;
    ByteReader rd(packed.bytes);
    CHECK(ReadBuildings(rd, back) && rd.AtEnd());
    const BuildingId id = back.Find(BOrigin::Lot, 0x1122334455ull);
    CHECK(id.Valid() && back.Get(id) && back.Get(id)->homes == 17 && back.Get(id)->jobSlots[4] == 13 && back.Get(id)->entranceNode == 40);
}

int main() {
    TestSnapshotAssemble();
    TestCrcAndBytes();
    TestRoundTrip();
    TestUnknownSkipped();
    TestNewerVersionFresh();
    TestOlderVersionLoads();
    TestBadCrcFresh();
    TestCorruptPayloadFresh();
    TestBadFiles();
    TestHead();
    TestRegistryStable();
    TestRegistryHints();
    TestTuningTables();
    if (failures) {
        std::printf("sim_save_test: %d FAILED\n", failures);
        return 1;
    }
    std::printf("sim_save_test: all passed\n");
    return 0;
}
