// The game's ten save slots (Save 1 .. Save 10): file names, the small metadata file beside each save, the bundled
// example city copied into Save 1 on first run, and the move of the old single-slot saves into the numbered slots.
// Platform free (std::filesystem only; no windows.h): city_game.inl writes and reads the saves themselves, the title
// screen (gui/title_screen.h, game/title_host.inl) lists and picks them. tests/save_slots_test.cpp covers it headless.
//
// Files, in the game's saves folder (Windows: <exe or project>\saves; iOS: Documents/bin/saves):
//   slot_01.tbdgame .. slot_10.tbdgame   the city (city_game.inl's plain text records)
//   slot_NN.hfd                          its terrain edits
//   slot_NN.meta                         what the slot list shows: key=value lines (below), written with every save
//   scratch.*                            test / stress saves (TBD_CITY_STRESS), never listed
//   .example_copied                      Save 1 was given the example city once (a deleted Save 1 stays empty)
// Older builds wrote city_slotN.tbdgame / .hfd (N = 1..10): MigrateLegacy copies each into slot N when that slot is
// empty and renames the old files to *.migrated (nothing is deleted).
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace tbd {
namespace saves {

constexpr int kSlots = 10;      // Save 1 .. Save 10: indices 0 .. 9
constexpr int kScratchSlot = 99; // test / stress saves (city_stress.inl): "scratch.*", never listed
constexpr int kAutosaveSlot0 = 90, kAutosaveSlots = 3; // the sim's rotating autosaves (city_sim_save.inl): "autosave_1" .. "autosave_3", never listed
constexpr int kMetaVersion = 1;

// "slot_01" .. "slot_10"; "scratch" for kScratchSlot.
inline std::string SlotStem(int slot) {
    if (slot == kScratchSlot) return "scratch";
    if (slot >= kAutosaveSlot0 && slot < kAutosaveSlot0 + kAutosaveSlots) return "autosave_" + std::to_string(slot - kAutosaveSlot0 + 1);
    char b[16];
    std::snprintf(b, sizeof(b), "slot_%02d", std::clamp(slot, 0, 98) + 1);
    return b;
}
inline std::string SlotLabel(int slot) { return "Save " + std::to_string(slot + 1); }

// What a slot shows without loading the city.
struct Meta {
    bool used = false;      // the slot has a .tbdgame
    std::string name;       // the city's / map's name ("Bayside")
    std::string map;        // the map, a line ("Bayside archipelago", "Random map, seed 1234")
    int64_t saved = 0;      // real time of the save, unix seconds (0: unknown)
    double minutes = 0;     // the game clock (game minutes since day 1, 00:00)
    long long population = -1; // -1 unknown
    long long money = 0;    // dollars
    bool example = false;   // the bundled example city, as shipped (not saved over yet)
};

inline std::string SerializeMeta(const Meta& m) {
    std::ostringstream o;
    o.precision(12);
    o << "version=" << kMetaVersion << "\nname=" << m.name << "\nmap=" << m.map << "\nsaved=" << m.saved << "\nminutes=" << m.minutes
      << "\npopulation=" << m.population << "\nmoney=" << m.money << "\nexample=" << (m.example ? 1 : 0) << "\n";
    return o.str();
}
// Unknown keys are skipped (newer files read); a missing key keeps its default.
inline bool ParseMeta(const std::string& text, Meta& m) {
    std::istringstream in(text);
    std::string line;
    bool any = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        any = true;
        if (k == "name") m.name = v;
        else if (k == "map") m.map = v;
        else if (k == "saved") m.saved = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "minutes") m.minutes = std::strtod(v.c_str(), nullptr);
        else if (k == "population") m.population = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "money") m.money = std::strtoll(v.c_str(), nullptr, 10);
        else if (k == "example") m.example = v == "1";
    }
    return any;
}

// The game clock as the dock shows it: "Mar 4 . Y1" (30-day months, 12 to a year; day 1 is Jan 1, Y1).
inline std::string GameDate(double minutes) {
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const long long day = std::max(0LL, (long long)(minutes / 1440.0));
    char b[40];
    std::snprintf(b, sizeof(b), "%s %lld, Y%lld", kMonths[(day / 30) % 12], day % 30 + 1, day / 360 + 1);
    return b;
}
// Real time: "Nov 8 2026, 14:32" (local time; utc for tests).
inline std::string RealDate(int64_t unix, bool utc = false) {
    if (unix <= 0) return "";
    const std::time_t t = (std::time_t)unix;
    std::tm tm{};
#if defined(_WIN32)
    if (utc) gmtime_s(&tm, &t); else localtime_s(&tm, &t);
#else
    if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
#endif
    static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[48];
    std::snprintf(b, sizeof(b), "%s %d %d, %02d:%02d", kMonths[tm.tm_mon % 12], tm.tm_mday, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    return b;
}
inline std::string Money(long long v) {
    char b[32];
    const long long a = v < 0 ? -v : v;
    if (a >= 1000000) std::snprintf(b, sizeof(b), "%s$%.2fM", v < 0 ? "-" : "", (double)a / 1e6);
    else if (a >= 10000) std::snprintf(b, sizeof(b), "%s$%lldk", v < 0 ? "-" : "", a / 1000);
    else std::snprintf(b, sizeof(b), "%s$%lld", v < 0 ? "-" : "", a);
    return b;
}
inline std::string Thousands(long long v) {
    std::string s = std::to_string(v < 0 ? -v : v), o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (i && (s.size() - i) % 3 == 0) o += ',';
        o += s[i];
    }
    return (v < 0 ? "-" : "") + o;
}

// A save's own records, for a slot without a .meta (an old save): the H record's clock and the M record's treasury.
inline Meta MetaFromSave(const std::string& text) {
    Meta m;
    m.used = true;
    std::istringstream in(text);
    std::string line;
    int mapKind = 2, seed = 0;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.size() < 2 || line[1] != '|') continue;
        std::vector<std::string> f;
        size_t a = 0;
        for (size_t b; (b = line.find('|', a)) != std::string::npos; a = b + 1) f.push_back(line.substr(a, b - a));
        f.push_back(line.substr(a));
        if (f[0] == "H" && f.size() >= 4) mapKind = std::atoi(f[1].c_str()), seed = std::atoi(f[2].c_str()), m.minutes = std::strtod(f[3].c_str(), nullptr);
        else if (f[0] == "M" && f.size() >= 3) m.money = std::strtoll(f[2].c_str(), nullptr, 10) / 100;
    }
    m.name = mapKind == 3 ? "Bayside" : "Shortville";
    m.map = mapKind == 3 ? "Bayside archipelago" : "Random map, seed " + std::to_string(seed);
    return m;
}

inline bool ReadFile(const std::filesystem::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    std::ostringstream s;
    s << in.rdbuf();
    out = s.str();
    return true;
}
inline bool WriteFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(text.data(), (std::streamsize)text.size());
    return (bool)o;
}

// The saves folder.
class Store {
public:
    explicit Store(std::filesystem::path dir) : dir_(std::move(dir)) {}
    const std::filesystem::path& Dir() const { return dir_; }
    std::filesystem::path File(int slot, const char* ext) const { return dir_ / (SlotStem(slot) + ext); }

    bool Used(int slot) const {
        std::error_code ec;
        return std::filesystem::is_regular_file(File(slot, ".tbdgame"), ec);
    }
    Meta Read(int slot) const {
        Meta m;
        if (!Used(slot)) return m;
        std::string text;
        if (ReadFile(File(slot, ".meta"), text) && ParseMeta(text, m)) {
            m.used = true;
            return m;
        }
        if (ReadFile(File(slot, ".tbdgame"), text)) m = MetaFromSave(text); // an old save without a .meta
        m.used = true;
        if (m.saved == 0) { // the file's time, then
            std::error_code ec;
            const auto ft = std::filesystem::last_write_time(File(slot, ".tbdgame"), ec);
            if (!ec) m.saved = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(ft.time_since_epoch()).count() - FileClockSkew();
        }
        return m;
    }
    std::vector<Meta> List() const {
        std::vector<Meta> v;
        for (int i = 0; i < kSlots; ++i) v.push_back(Read(i));
        return v;
    }
    bool WriteMeta(int slot, const Meta& m) const { return WriteFile(File(slot, ".meta"), SerializeMeta(m)); }
    // Removes the slot's files (the save, its terrain edits, its metadata). True when the slot is empty afterwards.
    bool Delete(int slot) const {
        std::error_code ec;
        for (const char* ext : {".tbdgame", ".hfd", ".meta"}) std::filesystem::remove(File(slot, ext), ec);
        return !Used(slot);
    }
    // The slot saved last (Continue), or -1 with no saves.
    int MostRecent() const {
        int best = -1;
        int64_t when = 0;
        for (int i = 0; i < kSlots; ++i) {
            const Meta m = Read(i);
            if (m.used && (best < 0 || m.saved > when)) best = i, when = m.saved;
        }
        return best;
    }
    int FirstEmpty() const {
        for (int i = 0; i < kSlots; ++i)
            if (!Used(i)) return i;
        return -1;
    }
    bool AnyUsed() const { return MostRecent() >= 0; }

    // The bundled example city into `slot` (Save 1) on first run: `from` holds <stem>.tbdgame (+ .hfd, .meta). Copies only
    // when the slot is empty and it was never done before (the marker file): a player's own Save 1 is never overwritten,
    // and a Save 1 the player deleted stays deleted. Returns true when it copied.
    bool CopyTemplateIfEmpty(int slot, const std::filesystem::path& from, const std::string& stem) const {
        std::error_code ec;
        const std::filesystem::path marker = dir_ / ".example_copied";
        if (std::filesystem::exists(marker, ec) || Used(slot)) return false;
        if (!std::filesystem::is_regular_file(from / (stem + ".tbdgame"), ec)) return false;
        std::filesystem::create_directories(dir_, ec);
        bool ok = true;
        for (const char* ext : {".tbdgame", ".hfd", ".meta"}) {
            const std::filesystem::path src = from / (stem + ext);
            if (!std::filesystem::is_regular_file(src, ec)) continue;
            // copy_options::none: fails rather than overwrite (a save that appeared since the check wins)
            ok = std::filesystem::copy_file(src, File(slot, ext), std::filesystem::copy_options::none, ec) && ok;
        }
        if (ok) {
            Meta m = Read(slot);
            m.example = true;
            if (m.saved == 0) m.saved = (int64_t)std::time(nullptr);
            WriteMeta(slot, m);
        }
        WriteFile(marker, "the example city was copied into Save 1 once\n");
        return ok;
    }

    // The example city on first run: into Save 1 when it is empty, else into the first empty slot (a player's old save that
    // moved into Save 1 keeps it, and the example is still there). Once ever (the marker). Returns the slot, or -1.
    int CopyTemplate(const std::filesystem::path& from, const std::string& stem) const {
        std::error_code ec;
        if (std::filesystem::exists(dir_ / ".example_copied", ec) || !std::filesystem::is_regular_file(from / (stem + ".tbdgame"), ec)) return -1;
        const int slot = Used(0) ? FirstEmpty() : 0;
        if (slot < 0) return -1;
        return CopyTemplateIfEmpty(slot, from, stem) ? slot : -1;
    }

    // The old single saves (city_slotN.tbdgame, N = 1..10) into the numbered slots. Each goes to slot N when that slot is
    // empty; the old files are renamed *.migrated (kept). Returns how many moved.
    int MigrateLegacy() const {
        int moved = 0;
        std::error_code ec;
        for (int n = 1; n <= kSlots; ++n) {
            const std::filesystem::path oldGame = dir_ / ("city_slot" + std::to_string(n) + ".tbdgame");
            if (!std::filesystem::is_regular_file(oldGame, ec)) continue;
            const int slot = n - 1;
            if (Used(slot)) continue; // both exist: the new one wins, the old one stays where it is
            const std::filesystem::path oldHfd = dir_ / ("city_slot" + std::to_string(n) + ".hfd");
            if (!std::filesystem::copy_file(oldGame, File(slot, ".tbdgame"), std::filesystem::copy_options::none, ec)) continue;
            if (std::filesystem::is_regular_file(oldHfd, ec)) std::filesystem::copy_file(oldHfd, File(slot, ".hfd"), std::filesystem::copy_options::none, ec);
            Meta m = Read(slot); // from the save's records and the file time
            WriteMeta(slot, m);
            std::filesystem::path g2 = oldGame, h2 = oldHfd;
            g2 += ".migrated", h2 += ".migrated";
            std::filesystem::rename(oldGame, g2, ec);
            if (std::filesystem::is_regular_file(oldHfd, ec)) std::filesystem::rename(oldHfd, h2, ec);
            ++moved;
        }
        return moved;
    }

private:
    // file_time_type's epoch is not the unix epoch on every library (MSVC: 1601). The offset, measured once.
    static int64_t FileClockSkew() {
        static const int64_t skew = [] {
            const auto f = std::filesystem::file_time_type::clock::now();
            const int64_t fs = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(f.time_since_epoch()).count();
            return fs - (int64_t)std::time(nullptr);
        }();
        return skew;
    }
    std::filesystem::path dir_;
};

} // namespace saves
} // namespace tbd
