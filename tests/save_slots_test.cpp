// Headless checks for the ten save slots (src/game/save_slots.h): the file names, the metadata round trip, ten slots that
// do not touch each other, the example city copied into Save 1 only when it is empty (and only once), and the old
// single saves (city_slotN) moved into the numbered slots without losing anything.
// Build and run: tests\save_slots_test.bat
#include "../src/game/save_slots.h"

#ifdef _WINDOWS_
#error "the save slots must not pull in windows.h"
#endif

#include <cstdio>

static int g_failed = 0, g_checks = 0;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        ++g_checks;                                                                    \
        if (!(cond)) { ++g_failed; std::printf("FAIL line %d: %s\n", __LINE__, #cond); } \
    } while (0)

namespace fs = std::filesystem;
using namespace tbd::saves;

static fs::path Fresh(const char* name) {
    const fs::path p = fs::temp_directory_path() / "tbd_save_slots_test" / name;
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p, ec);
    return p;
}
static std::string Read(const fs::path& p) {
    std::string s;
    ReadFile(p, s);
    return s;
}
static std::string FakeSave(int seed, double minutes, long long cents, int mapKind = 2) {
    return "TBDGAME 1\nH|" + std::to_string(mapKind) + "|" + std::to_string(seed) + "|" + std::to_string(minutes) + "|1|0 0 0|3000|1.1|0|0|0|0|0\nT|0.3\nM|1|" +
           std::to_string(cents) + "|90 90 90 90|100 100 100 100 100 100|0|0 0 0 0|0 0 0\n";
}

int main() {
    // ---- names
    CHECK(SlotStem(0) == "slot_01" && SlotStem(9) == "slot_10" && SlotStem(kScratchSlot) == "scratch");
    CHECK(SlotLabel(0) == "Save 1" && SlotLabel(9) == "Save 10");
    for (int i = 0; i < kSlots; ++i)
        for (int j = i + 1; j < kSlots; ++j) CHECK(SlotStem(i) != SlotStem(j));

    // ---- metadata round trip
    {
        Meta m;
        m.used = true, m.name = "Bayside", m.map = "Bayside archipelago", m.saved = 1762600000, m.minutes = 12345.5, m.population = 4321, m.money = 1500000, m.example = true;
        Meta back;
        CHECK(ParseMeta(SerializeMeta(m), back));
        CHECK(back.name == m.name && back.map == m.map && back.saved == m.saved && back.minutes == m.minutes && back.population == m.population && back.money == m.money && back.example);
        Meta part; // unknown keys skipped, missing keys keep defaults, CRLF tolerated
        CHECK(ParseMeta("name=X\r\nfuture=1\r\nmoney=-50\r\n", part) && part.name == "X" && part.money == -50 && part.population == -1 && part.saved == 0);
        CHECK(!ParseMeta("", part));
        CHECK(GameDate(0) == "Jan 1, Y1" && GameDate(1440.0 * 30) == "Feb 1, Y1" && GameDate(1440.0 * 365) == "Jan 6, Y2");
        CHECK(RealDate(0).empty() && RealDate(1762600000, true) == "Nov 8 2025, 11:06");
        CHECK(Money(1500000) == "$1.50M" && Money(-12000) == "-$12k" && Money(950) == "$950" && Thousands(1234567) == "1,234,567");
        const Meta f = MetaFromSave(FakeSave(77, 3000, 123456789));
        CHECK(f.used && f.minutes == 3000 && f.money == 1234567 && f.map == "Random map, seed 77" && f.name == "Shortville");
        CHECK(MetaFromSave(FakeSave(2026, 0, 0, 3)).name == "Bayside");
    }

    // ---- ten independent slots
    {
        const fs::path dir = Fresh("ten");
        const Store st(dir);
        CHECK(st.MostRecent() == -1 && st.FirstEmpty() == 0 && !st.AnyUsed());
        for (int i = 0; i < kSlots; ++i) {
            CHECK(WriteFile(st.File(i, ".tbdgame"), FakeSave(i, i * 100.0, i * 100)));
            Meta m;
            m.name = "City " + std::to_string(i), m.saved = 1000 + i * (i == 4 ? 50 : 1), m.population = i;
            CHECK(st.WriteMeta(i, m));
        }
        CHECK(st.FirstEmpty() == -1);
        const std::vector<Meta> list = st.List();
        CHECK(list.size() == (size_t)kSlots);
        for (int i = 0; i < kSlots; ++i) CHECK(list[(size_t)i].used && list[(size_t)i].name == "City " + std::to_string(i) && list[(size_t)i].population == i);
        CHECK(st.MostRecent() == 4); // saved last
        CHECK(st.Delete(4) && !st.Used(4) && !fs::exists(st.File(4, ".meta")));
        for (int i = 0; i < kSlots; ++i) CHECK(st.Used(i) == (i != 4));
        CHECK(st.MostRecent() == 9 && st.FirstEmpty() == 4);
        // overwrite one: the others keep their files
        CHECK(WriteFile(st.File(2, ".tbdgame"), FakeSave(999, 1, 1)));
        CHECK(Read(st.File(3, ".tbdgame")) == FakeSave(3, 300.0, 300));
        // a save without a .meta reads from its records
        std::error_code ec;
        fs::remove(st.File(7, ".meta"), ec);
        const Meta m7 = st.Read(7);
        CHECK(m7.used && m7.minutes == 700 && m7.saved > 0);
        // the scratch slot is never listed
        CHECK(WriteFile(st.File(kScratchSlot, ".tbdgame"), FakeSave(1, 1, 1)));
        int used = 0;
        for (const Meta& m : st.List()) used += m.used;
        CHECK(used == 9);
    }

    // ---- the example city into Save 1: only when empty, only once
    {
        const fs::path tpl = Fresh("template");
        CHECK(WriteFile(tpl / "bayside.tbdgame", FakeSave(2026, 600, 150000000, 3)));
        CHECK(WriteFile(tpl / "bayside.hfd", "HFD"));
        Meta tm;
        tm.name = "Bayside", tm.population = 900, tm.example = true;
        CHECK(WriteFile(tpl / "bayside.meta", SerializeMeta(tm)));
        {
            const Store st(Fresh("fresh"));
            CHECK(st.CopyTemplateIfEmpty(0, tpl, "bayside"));
            CHECK(st.Used(0) && Read(st.File(0, ".hfd")) == "HFD");
            const Meta m = st.Read(0);
            CHECK(m.name == "Bayside" && m.example && m.population == 900 && m.saved > 0);
            CHECK(st.MostRecent() == 0);
            // deleted by the player: stays deleted
            CHECK(st.Delete(0));
            CHECK(!st.CopyTemplateIfEmpty(0, tpl, "bayside") && !st.Used(0));
        }
        {
            const Store st(Fresh("own"));
            CHECK(WriteFile(st.File(0, ".tbdgame"), FakeSave(5, 5, 5)));
            CHECK(!st.CopyTemplateIfEmpty(0, tpl, "bayside"));
            CHECK(Read(st.File(0, ".tbdgame")) == FakeSave(5, 5, 5)); // the player's Save 1 untouched
        }
        { // the host's call: Save 1 when empty, else the first empty slot, once
            const Store st(Fresh("host_fresh"));
            CHECK(st.CopyTemplate(tpl, "bayside") == 0 && st.Used(0));
            CHECK(st.Delete(0) && st.CopyTemplate(tpl, "bayside") == -1);
            const Store own(Fresh("host_own"));
            CHECK(WriteFile(own.File(0, ".tbdgame"), FakeSave(5, 5, 5)) && WriteFile(own.File(1, ".tbdgame"), FakeSave(6, 6, 6)));
            CHECK(own.CopyTemplate(tpl, "bayside") == 2 && own.Read(2).example && Read(own.File(0, ".tbdgame")) == FakeSave(5, 5, 5));
            CHECK(own.CopyTemplate(tpl, "bayside") == -1);
            const Store full(Fresh("host_full"));
            for (int i = 0; i < kSlots; ++i) CHECK(WriteFile(full.File(i, ".tbdgame"), FakeSave(i, 1, 1)));
            CHECK(full.CopyTemplate(tpl, "bayside") == -1);
        }
        {
            const Store st(Fresh("notemplate"));
            CHECK(!st.CopyTemplateIfEmpty(0, Fresh("empty_template"), "bayside") && !st.Used(0));
        }
    }

    // ---- the old single saves move in
    {
        const fs::path dir = Fresh("legacy");
        const Store st(dir);
        CHECK(WriteFile(dir / "city_slot1.tbdgame", FakeSave(11, 100, 500)));
        CHECK(WriteFile(dir / "city_slot1.hfd", "OLDHFD"));
        CHECK(WriteFile(dir / "city_slot10.tbdgame", FakeSave(10, 200, 600)));
        CHECK(WriteFile(dir / "city_slot3.tbdgame", FakeSave(33, 1, 1)));
        CHECK(WriteFile(st.File(2, ".tbdgame"), FakeSave(3, 3, 3))); // Save 3 already in use: the old one stays put
        CHECK(st.MigrateLegacy() == 2);
        CHECK(st.Used(0) && Read(st.File(0, ".tbdgame")) == FakeSave(11, 100, 500) && Read(st.File(0, ".hfd")) == "OLDHFD");
        CHECK(st.Read(0).map == "Random map, seed 11" && st.Read(0).money == 5);
        CHECK(st.Used(9) && Read(st.File(9, ".tbdgame")) == FakeSave(10, 200, 600));
        CHECK(fs::exists(dir / "city_slot1.tbdgame.migrated") && fs::exists(dir / "city_slot1.hfd.migrated") && !fs::exists(dir / "city_slot1.tbdgame"));
        CHECK(Read(st.File(2, ".tbdgame")) == FakeSave(3, 3, 3) && fs::exists(dir / "city_slot3.tbdgame")); // nothing lost
        CHECK(st.MigrateLegacy() == 0); // idempotent
        // and the example then does not overwrite the migrated Save 1
        const fs::path tpl = Fresh("template2");
        CHECK(WriteFile(tpl / "bayside.tbdgame", FakeSave(2026, 1, 1, 3)));
        CHECK(!st.CopyTemplateIfEmpty(0, tpl, "bayside") && Read(st.File(0, ".tbdgame")) == FakeSave(11, 100, 500));
    }

    std::error_code ec;
    fs::remove_all(fs::temp_directory_path() / "tbd_save_slots_test", ec);
    std::printf("save_slots_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
