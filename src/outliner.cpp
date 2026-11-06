#define UNICODE
#define _UNICODE
#include "outliner.h"

#include <windowsx.h>
#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "ui_theme.h"

namespace tbd {
namespace {

const wchar_t* kOutlinerClass = L"TBDOutliner";
constexpr UINT_PTR kPollTimer = 1;

// Text converted once per rebuild (not once per painted row).
struct Entry {
    std::wstring name;   // the full entity name, "Doors/Saloon Door"
    std::wstring shown;  // what the row shows: the part after the first "/"
    std::wstring folder; // the part before it ("" = not in a folder)
    std::wstring kind;
    int group = -1;      // index into State::groups
};

// A folder: entities whose names start with "<label>/". `key` is the lower-case label, so "Doors" and "doors" merge.
struct Group {
    std::wstring key, label;
    std::vector<int> members; // entity indices that pass the search, in scene order
};

struct Row {
    int entity = -1; // < 0: a folder header row
    int group = -1;  // the folder the row belongs to (-1 = loose entity)
};

struct State {
    HWND hwnd = nullptr;
    HWND search = nullptr;
    int dpi = 96;
    HFONT font = nullptr, small = nullptr, bold = nullptr;
    HBRUSH editBrush = nullptr;
    OutlinerSource src;
    std::wstring filter;
    std::vector<Entry> entries;       // one per entity
    std::vector<Group> groups;        // folders, sorted by name
    std::vector<int> loose;           // entities that pass the search and are in no folder
    std::set<std::wstring> collapsed; // keys of the folders the user closed
    std::vector<Row> rows;            // what is listed right now: folders, then their open contents, then loose entities
    int total = 0;                    // entities in the scene
    int selected = -1;     // entity index
    int hover = -1;        // row
    int scroll = 0;
    bool tracking = false;
    bool suppressSearch = false;
    unsigned signature = 0;
};

int S(const State& s, int v) { return MulDiv(v, s.dpi, 96); }

// ----------------------------------------------------------------- helpers

std::wstring Wide(const char* text) {
    if (!text || !*text) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (n <= 1) return L"";
    std::wstring out((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, &out[0], n);
    return out;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

const char* NameOf(const State& s, int i) { return s.src.name ? s.src.name(i, s.src.user) : ""; }
const char* KindOf(const State& s, int i) { return s.src.kind ? s.src.kind(i, s.src.user) : ""; }
int CountOf(const State& s) { return s.src.count ? s.src.count(s.src.user) : 0; }

// ---------------------------------------------------------------- geometry

int TabH(const State& s) { return S(s, 26); }
int SearchH(const State& s) { return S(s, 36); }
int ColH(const State& s) { return S(s, 20); }
int FootH(const State& s) { return S(s, 22); }
int RowH(const State& s) { return S(s, 22); }

RECT ListRect(const State& s, int w, int h) {
    const int top = TabH(s) + SearchH(s) + ColH(s);
    return {0, top, w, std::max(top, h - FootH(s))};
}

RECT SearchFrame(const State& s, int w) {
    const int top = TabH(s) + S(s, 5);
    return {S(s, 8), top, w - S(s, 8), top + S(s, 26)};
}

int MaxScroll(const State& s, int w, int h) {
    const RECT l = ListRect(s, w, h);
    return std::max(0, (int)s.rows.size() * RowH(s) - (int)(l.bottom - l.top));
}

void ClampScroll(State& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    s.scroll = std::clamp(s.scroll, 0, MaxScroll(s, rc.right, rc.bottom));
}

int RowAt(const State& s, int x, int y) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT l = ListRect(s, rc.right, rc.bottom);
    const POINT pt = {x, y};
    if (!PtInRect(&l, pt)) return -1;
    const int r = (y - l.top + s.scroll) / RowH(s);
    return r >= 0 && r < (int)s.rows.size() ? r : -1;
}

int RowOf(const State& s, int entity) {
    if (entity < 0) return -1;
    for (int r = 0; r < (int)s.rows.size(); ++r)
        if (s.rows[r].entity == entity) return r;
    return -1;
}

// A folder is open unless the user closed it; while searching, every folder with a hit stays open.
bool GroupOpen(const State& s, int g) { return !s.filter.empty() || s.collapsed.count(s.groups[g].key) == 0; }

// Flattens the folders and loose entities into the rows that are listed (cheap: no text work).
void BuildRows(State& s) {
    s.rows.clear();
    for (int g = 0; g < (int)s.groups.size(); ++g) {
        const Group& grp = s.groups[g];
        if (grp.members.empty()) continue;
        s.rows.push_back({-1, g});
        if (GroupOpen(s, g))
            for (int e : grp.members) s.rows.push_back({e, g});
    }
    for (int e : s.loose) s.rows.push_back({e, -1});
    ClampScroll(s);
}

void ToggleGroup(State& s, int g) {
    if (g < 0 || g >= (int)s.groups.size() || !s.filter.empty()) return;
    const std::wstring& key = s.groups[g].key;
    if (!s.collapsed.erase(key)) s.collapsed.insert(key);
    BuildRows(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void EnsureVisible(State& s, int entity) {
    if (entity < 0) return;
    int r = RowOf(s, entity);
    if (r < 0 && entity < (int)s.entries.size()) { // inside a closed folder: open it
        const int g = s.entries[entity].group;
        if (g >= 0 && g < (int)s.groups.size() && s.collapsed.erase(s.groups[g].key)) {
            BuildRows(s);
            r = RowOf(s, entity);
        }
    }
    if (r < 0) return;
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT l = ListRect(s, rc.right, rc.bottom);
    const int top = r * RowH(s), bottom = top + RowH(s), view = l.bottom - l.top;
    if (top < s.scroll) s.scroll = top;
    else if (bottom > s.scroll + view) s.scroll = bottom - view;
    ClampScroll(s);
}

// ----------------------------------------------------------------- content

// Converts every entity's text once, sorts the entities that pass the search into folders (the part of the name
// before the first "/") and loose entities, then builds the rows.
void Rebuild(State& s) {
    s.total = CountOf(s);
    s.entries.assign((size_t)s.total, Entry());
    s.groups.clear();
    s.loose.clear();
    const std::wstring needle = Lower(s.filter);

    std::map<std::wstring, Group> byKey; // ordered by the lower-case folder name
    for (int i = 0; i < s.total; ++i) {
        Entry& e = s.entries[i];
        e.name = Wide(NameOf(s, i));
        e.kind = Wide(KindOf(s, i));
        e.shown = e.name;
        const size_t slash = e.name.find(L'/');
        if (slash != std::wstring::npos && slash > 0 && slash + 1 < e.name.size()) {
            e.folder = e.name.substr(0, slash);
            e.shown = e.name.substr(slash + 1);
        }
        if (!needle.empty() && Lower(e.name).find(needle) == std::wstring::npos) continue;
        if (e.folder.empty()) {
            s.loose.push_back(i);
        } else {
            Group& g = byKey[Lower(e.folder)];
            if (g.members.empty()) {
                g.key = Lower(e.folder);
                g.label = e.folder;
            }
            g.members.push_back(i);
        }
    }
    for (auto& kv : byKey) s.groups.push_back(std::move(kv.second));
    for (int g = 0; g < (int)s.groups.size(); ++g)
        for (int e : s.groups[g].members) s.entries[e].group = g;
    BuildRows(s);
}

// Cheap fingerprint of the scene's entity list (count, names, types), to know when to rebuild the rows.
unsigned Signature(const State& s) {
    unsigned h = 2166136261u;
    auto mix = [&](unsigned v) { h = (h ^ v) * 16777619u; };
    const int n = CountOf(s);
    mix((unsigned)n);
    for (int i = 0; i < n; ++i) {
        for (const char* p = NameOf(s, i); p && *p; ++p) mix((unsigned char)*p);
        mix(0xFFu);
        for (const char* p = KindOf(s, i); p && *p; ++p) mix((unsigned char)*p);
        mix(0xFEu);
    }
    return h;
}

void Poll(State& s) {
    bool dirty = false;
    const unsigned sig = Signature(s);
    if (sig != s.signature) {
        s.signature = sig;
        Rebuild(s);
        dirty = true;
    }
    const int sel = s.src.selected ? s.src.selected(s.src.user) : -1;
    if (sel != s.selected) {
        s.selected = sel;
        EnsureVisible(s, sel);
        dirty = true;
    }
    if (dirty) InvalidateRect(s.hwnd, nullptr, FALSE);
}

void Select(State& s, int entity) {
    if (s.src.select) s.src.select(entity, s.src.user);
    s.selected = s.src.selected ? s.src.selected(s.src.user) : entity;
    EnsureVisible(s, s.selected);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void Run(State& s, OutlinerCommand command) {
    if (s.src.command) s.src.command(command, s.src.user);
    Poll(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

// ---------------------------------------------------------------- painting

// Section header text whose last letter ends exactly at `right`. DT_RIGHT is not used: with letter spacing on, GDI
// right-aligns by the width without the spacing, so the last letter ("TYPE" -> "TYP") ends up outside the rect.
void DrawCapsEndingAt(HDC dc, int right, int top, int bottom, const wchar_t* text) {
    SetTextCharacterExtra(dc, 1);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SetTextCharacterExtra(dc, 0);
    const int left = right - sz.cx;
    theme::DrawCaps(dc, {left, top, right + 8, bottom}, text);
}

// Invalidates one list row (hover changes only repaint the rows involved).
void InvalidateRow(const State& s, int row) {
    if (row < 0) return;
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT list = ListRect(s, rc.right, rc.bottom);
    const int y = list.top + row * RowH(s) - s.scroll;
    RECT r = {0, std::max((int)list.top, y), rc.right, std::min((int)list.bottom, y + RowH(s))};
    if (r.bottom > r.top) InvalidateRect(s.hwnd, &r, FALSE);
}

void Paint(HWND hwnd, State& s) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right, h = client.bottom;
    const RECT pr = ps.rcPaint;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldFont = SelectObject(mem, s.font);
    SetBkMode(mem, TRANSPARENT);
    IntersectClipRect(mem, pr.left, pr.top, pr.right, pr.bottom); // only the dirty part is redrawn
    theme::Fill(mem, client, theme::kPanel);
    ClampScroll(s);

    // Tab strip with the panel's title.
    theme::Fill(mem, {0, 0, w, TabH(s)}, theme::kWindow);
    SelectObject(mem, s.bold);
    const int titleW = 78 * s.dpi / 96;
    theme::Fill(mem, {S(s, 6), S(s, 4), S(s, 6) + titleW + S(s, 20), TabH(s)}, theme::kPanel);
    SetTextColor(mem, theme::kTextBright);
    theme::DrawCaps(mem, {S(s, 16), S(s, 4), S(s, 16) + titleW + S(s, 10), TabH(s)}, L"OUTLINER");

    // Search box.
    const RECT frame = SearchFrame(s, w);
    theme::Fill(mem, frame, theme::kWindow);
    theme::Frame(mem, frame, GetFocus() == s.search ? theme::kAccent : theme::kRule);
    theme::DrawMagnifier(mem, frame.left + S(s, 8), frame.top + S(s, 8), S(s, 8), std::max(1, S(s, 1)), theme::kMuted);

    // Column header.
    const int colTop = TabH(s) + SearchH(s);
    theme::Fill(mem, {0, colTop + ColH(s) - 1, w, colTop + ColH(s)}, theme::kRule);
    SetTextColor(mem, theme::kMuted);
    theme::DrawCaps(mem, {S(s, 32), colTop, w / 2, colTop + ColH(s)}, L"ITEM LABEL");
    DrawCapsEndingAt(mem, w - S(s, 12), colTop, colTop + ColH(s) - 1, L"TYPE");

    // Rows, clipped to the list.
    const RECT list = ListRect(s, w, h);
    const int saved = SaveDC(mem);
    IntersectClipRect(mem, list.left, list.top, list.right, list.bottom);
    const int rowH = RowH(s);
    const int first = std::max(0, s.scroll / rowH);
    for (int r = first; r < (int)s.rows.size(); ++r) {
        const int y = list.top + r * rowH - s.scroll;
        if (y >= list.bottom || y >= pr.bottom) break;
        if (y + rowH <= pr.top) continue;
        const Row& rw = s.rows[r];
        const bool hot = r == s.hover;
        const RECT row = {0, y, w, y + rowH};
        RECT kind = {w - S(s, 88), y, w - S(s, 12), y + rowH};

        if (rw.entity < 0) { // folder header: arrow, folder glyph, name, item count
            const Group& g = s.groups[rw.group];
            theme::Fill(mem, row, hot ? theme::kHover : theme::kRaised);
            theme::DrawDisclosure(mem, S(s, 14), y + rowH / 2, S(s, 4), GroupOpen(s, rw.group), theme::kMuted);
            const COLORREF fc = hot ? theme::kText : theme::kMuted;
            const int fx = S(s, 24), fy = y + (rowH - S(s, 10)) / 2;
            theme::Fill(mem, {fx, fy, fx + S(s, 5), fy + S(s, 3)}, fc);
            theme::Fill(mem, {fx, fy + S(s, 2), fx + S(s, 12), fy + S(s, 10)}, fc);
            SelectObject(mem, s.font);
            SetTextColor(mem, theme::kTextBright);
            RECT name = {S(s, 44), y, w - S(s, 90), y + rowH};
            DrawTextW(mem, g.label.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(mem, s.small);
            SetTextColor(mem, theme::kMuted);
            const std::wstring count = std::to_wstring(g.members.size()) + (g.members.size() == 1 ? L" item" : L" items");
            DrawTextW(mem, count.c_str(), -1, &kind, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
            continue;
        }

        const int entity = rw.entity;
        if (entity >= (int)s.entries.size()) continue; // the scene changed since the last rebuild
        const Entry& e = s.entries[entity];
        const bool sel = entity == s.selected;
        theme::Fill(mem, row, sel ? theme::kAccent : (hot ? theme::kHover : (r % 2 ? theme::kStripe : theme::kPanel)));
        const int indent = rw.group >= 0 ? S(s, 18) : 0;

        // Small outlined cube as the type icon.
        const COLORREF iconColor = sel ? RGB(255, 255, 255) : theme::kMuted;
        const int ix = S(s, 12) + indent, iy = y + (rowH - S(s, 11)) / 2;
        theme::Frame(mem, {ix, iy + S(s, 3), ix + S(s, 8), iy + S(s, 11)}, iconColor);
        theme::Frame(mem, {ix + S(s, 3), iy, ix + S(s, 11), iy + S(s, 8)}, iconColor);

        SelectObject(mem, s.font);
        SetTextColor(mem, sel ? RGB(255, 255, 255) : theme::kText);
        RECT name = {S(s, 32) + indent, y, w - S(s, 90), y + rowH};
        DrawTextW(mem, e.shown.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

        SelectObject(mem, s.small);
        SetTextColor(mem, sel ? RGB(220, 235, 255) : theme::kMuted);
        DrawTextW(mem, e.kind.c_str(), -1, &kind, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (s.rows.empty()) {
        SelectObject(mem, s.font);
        SetTextColor(mem, theme::kMuted);
        RECT msg = list;
        DrawTextW(mem, s.filter.empty() ? L"No entities in the scene." : L"Nothing matches your search.", -1, &msg,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    RestoreDC(mem, saved);

    // Scrollbar thumb.
    const int totalH = (int)s.rows.size() * rowH, view = list.bottom - list.top;
    if (totalH > view && view > 0) {
        const int thumbH = std::max(S(s, 24), view * view / totalH);
        const int maxScroll = totalH - view;
        const int thumbY = list.top + (view - thumbH) * s.scroll / std::max(1, maxScroll);
        theme::Fill(mem, {w - S(s, 6), thumbY, w - S(s, 2), thumbY + thumbH}, theme::kHover);
    }

    // Footer: how many entities, how many selected.
    theme::Fill(mem, {0, list.bottom, w, h}, theme::kWindow);
    theme::Fill(mem, {0, list.bottom, w, list.bottom + 1}, theme::kRule);
    size_t matching = s.loose.size();
    for (const Group& g : s.groups) matching += g.members.size();
    std::wstring status = s.filter.empty() ? std::to_wstring(s.total) + L" entities"
                                           : std::to_wstring(matching) + L" of " + std::to_wstring(s.total) + L" entities";
    if (s.selected >= 0) status += L" (1 selected)";
    SelectObject(mem, s.small);
    SetTextColor(mem, theme::kMuted);
    RECT st = {S(s, 10), list.bottom, w - S(s, 10), h};
    DrawTextW(mem, status.c_str(), -1, &st, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    theme::Fill(mem, {0, 0, 1, h}, theme::kLine); // edge towards the viewport

    BitBlt(dc, pr.left, pr.top, pr.right - pr.left, pr.bottom - pr.top, mem, pr.left, pr.top, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- input

void Layout(State& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT f = SearchFrame(s, rc.right);
    MoveWindow(s.search, f.left + S(s, 24), f.top + S(s, 5), std::max(10, (int)(f.right - f.left) - S(s, 30)), S(s, 16), TRUE);
    ClampScroll(s);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void ContextMenu(State& s, POINT screen) {
    enum { kFrame = 1, kDuplicate, kDelete };
    const bool has = s.selected >= 0;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (has ? 0 : MF_GRAYED), kFrame, L"Frame\tF");
    AppendMenuW(menu, MF_STRING | (has ? 0 : MF_GRAYED), kDuplicate, L"Duplicate\tCtrl+D");
    AppendMenuW(menu, MF_STRING | (has ? 0 : MF_GRAYED), kDelete, L"Delete\tDel");
    const int cmd = theme::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, s.hwnd, s.dpi);
    DestroyMenu(menu);
    switch (cmd) {
    case kFrame: Run(s, OutlinerCommand::Frame); break;
    case kDuplicate: Run(s, OutlinerCommand::Duplicate); break;
    case kDelete: Run(s, OutlinerCommand::Delete); break;
    }
}

void OnLeftDown(State& s, int x, int y, bool doubleClick) {
    SetFocus(s.hwnd);
    const int r = RowAt(s, x, y);
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT list = ListRect(s, rc.right, rc.bottom);
    const POINT pt = {x, y};
    if (r >= 0) {
        const Row row = s.rows[r];
        if (row.entity < 0) { // a folder: one click opens or closes it (the click that starts a double-click already did)
            if (!doubleClick) ToggleGroup(s, row.group);
            return;
        }
        Select(s, row.entity);
        if (doubleClick) Run(s, OutlinerCommand::Frame); // same Frame as the toolbar button and the F key
    } else if (PtInRect(&list, pt)) {
        Select(s, -1); // empty space below the last row
    }
}

LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    State* s = reinterpret_cast<State*>(ref);
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        SetWindowTextW(hwnd, L"");
        SetFocus(s->hwnd);
        return 0;
    }
    if (msg == WM_CHAR && wparam == VK_ESCAPE) return 0; // no beep
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK OutlinerProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_MEASUREITEM:
    case WM_DRAWITEM: { // the dark popup menus (see ui_theme.h)
        LRESULT handled = 0;
        if (theme::HandleMenuMessage(msg, lparam, &handled)) return handled;
        break;
    }
    case WM_PAINT:
        if (s) Paint(hwnd, *s);
        else ValidateRect(hwnd, nullptr);
        return 0;
    case WM_SIZE:
        if (s && s->search) Layout(*s);
        return 0;
    case WM_TIMER:
        if (s && wparam == kPollTimer) Poll(*s);
        return 0;
    case WM_COMMAND:
        if (s && (HWND)lparam == s->search) {
            if (HIWORD(wparam) == EN_CHANGE && !s->suppressSearch) {
                wchar_t buf[128] = {};
                GetWindowTextW(s->search, buf, 128);
                s->filter = buf;
                s->scroll = 0;
                Rebuild(*s);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS) {
                InvalidateRect(hwnd, nullptr, FALSE); // the frame turns blue while typing
            }
        }
        return 0;
    case WM_CTLCOLOREDIT:
        SetTextColor((HDC)wparam, theme::kText);
        SetBkColor((HDC)wparam, theme::kWindow);
        return s ? (LRESULT)s->editBrush : 0;
    case WM_LBUTTONDOWN:
        if (s) OnLeftDown(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (s) OnLeftDown(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
        return 0;
    case WM_RBUTTONDOWN:
        if (s) {
            SetFocus(hwnd);
            const int r = RowAt(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (r >= 0 && s->rows[r].entity >= 0) Select(*s, s->rows[r].entity);
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(hwnd, &pt);
            ContextMenu(*s, pt);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s) {
            const int r = RowAt(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (r != s->hover) {
                InvalidateRow(*s, s->hover);
                s->hover = r;
                InvalidateRow(*s, r);
            }
            if (!s->tracking) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
                s->tracking = TrackMouseEvent(&t) != FALSE;
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (s) {
            s->tracking = false;
            InvalidateRow(*s, s->hover);
            s->hover = -1;
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (s) {
            s->scroll -= GET_WHEEL_DELTA_WPARAM(wparam) * S(*s, 66) / WHEEL_DELTA;
            ClampScroll(*s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (s) {
            switch (wparam) {
            case VK_UP:
            case VK_DOWN: {
                const int dir = wparam == VK_UP ? -1 : 1, n = (int)s->rows.size();
                const int cur = RowOf(*s, s->selected);
                const int start = cur < 0 ? (dir > 0 ? 0 : n - 1) : cur + dir;
                for (int i = start; i >= 0 && i < n; i += dir) // folder rows are skipped
                    if (s->rows[i].entity >= 0) {
                        Select(*s, s->rows[i].entity);
                        break;
                    }
                return 0;
            }
            case VK_DELETE: Run(*s, OutlinerCommand::Delete); return 0;
            case VK_ESCAPE: Select(*s, -1); return 0;
            case 'F':
                if (!(GetKeyState(VK_CONTROL) & 0x8000)) {
                    Run(*s, OutlinerCommand::Frame);
                    return 0;
                }
                break;
            }
        }
        break;
    case WM_NCDESTROY:
        KillTimer(hwnd, kPollTimer);
        if (s) {
            DeleteObject(s->font);
            DeleteObject(s->small);
            DeleteObject(s->bold);
            DeleteObject(s->editBrush);
            delete s;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

HWND Outliner_Create(HWND parent, HINSTANCE instance, int dpi, const OutlinerSource& source) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = OutlinerProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kOutlinerClass;
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }

    auto* s = new State;
    s->dpi = dpi;
    s->src = source;
    s->editBrush = CreateSolidBrush(theme::kWindow);
    s->font = theme::MakeFont(dpi, 12, FW_NORMAL);
    s->small = theme::MakeFont(dpi, 11, FW_NORMAL);
    s->bold = theme::MakeFont(dpi, 11, FW_SEMIBOLD);

    HWND hwnd = CreateWindowExW(0, kOutlinerClass, L"Outliner", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 100, 100,
                                parent, nullptr, instance, s);
    if (!hwnd) {
        DeleteObject(s->font);
        DeleteObject(s->small);
        DeleteObject(s->bold);
        DeleteObject(s->editBrush);
        delete s;
        return nullptr;
    }
    s->hwnd = hwnd;

    s->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 100, 16, hwnd, nullptr,
                                instance, nullptr);
    SendMessageW(s->search, WM_SETFONT, (WPARAM)s->font, TRUE);
    SendMessageW(s->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search");
    SendMessageW(s->search, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
    SetWindowSubclass(s->search, SearchProc, 1, (DWORD_PTR)s);

    s->signature = Signature(*s) + 1u; // differs from the first poll's, so the rows get built right away
    Layout(*s);
    SetTimer(hwnd, kPollTimer, 100, nullptr);
    Poll(*s);
    return hwnd;
}

} // namespace tbd
