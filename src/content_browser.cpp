#define UNICODE
#define _UNICODE
#include "content_browser.h"

#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <string>
#include <vector>

#include "ui_theme.h"

namespace tbd {
namespace {

const wchar_t* kBrowserClass = L"TBDContentBrowser";

const COLORREF kBg = theme::kWindow;       // asset grid
const COLORREF kHeaderBg = theme::kWindow; // tab strip and footer
const COLORREF kSideBg = theme::kPanel;    // folder tree
const COLORREF kHover = theme::kHover;
const COLORREF kAccent = theme::kAccent;
const COLORREF kText = theme::kText;
const COLORREF kMuted = theme::kMuted;
const COLORREF kBorder = theme::kRule;

constexpr UINT_PTR kWatchTimer = 1;
constexpr UINT kMsgEndRename = WM_APP + 1; // wParam: 1 = commit, 0 = cancel

enum class Kind { Folder, Model, Texture, Scene, Script, Audio, Shader, Other };

struct Entry {
    std::wstring name;
    std::wstring path;
    bool dir = false;
    Kind kind = Kind::Other;
    ULONGLONG size = 0;
};

struct TreeNode {
    std::wstring path;
    std::wstring name;
    int depth;
};

struct Crumb {
    RECT rc;
    std::wstring path;
    std::wstring label;
};

struct State {
    HWND hwnd = nullptr;
    HWND search = nullptr;
    HWND rename = nullptr;
    int dpi = 96;
    HFONT font = nullptr, small = nullptr, bold = nullptr;
    HBRUSH editBrush = nullptr;

    std::wstring root, current;
    std::vector<std::wstring> history;
    std::vector<Entry> entries;
    std::vector<TreeNode> tree;
    std::vector<std::wstring> collapsed; // folders whose children are hidden in the tree
    std::wstring filter;
    bool suppressSearch = false;

    int selected = -1, hover = -1;
    int scroll = 0;
    int tile = 88; // device-independent px
    int renaming = -1;
    bool tracking = false;
    FILETIME stamp = {};
    ContentBrowserCallbacks cb;
};

int S(const State& s, int v) { return MulDiv(v, s.dpi, 96); }

// ----------------------------------------------------------------- helpers

std::wstring Join(const std::wstring& a, const std::wstring& b) {
    return (a.empty() || a.back() == L'\\') ? a + b : a + L'\\' + b;
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring Extension(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    return dot == std::wstring::npos ? L"" : Lower(name.substr(dot));
}

Kind KindFor(const std::wstring& name) {
    const std::wstring e = Extension(name);
    auto in = [&](std::initializer_list<const wchar_t*> list) {
        for (const wchar_t* x : list)
            if (e == x) return true;
        return false;
    };
    if (in({L".obj", L".fbx", L".gltf", L".glb", L".dae", L".stl"})) return Kind::Model;
    if (in({L".png", L".jpg", L".jpeg", L".tga", L".bmp", L".dds", L".hdr", L".tif", L".tiff"})) return Kind::Texture;
    if (in({L".tbdscene"})) return Kind::Scene;
    if (in({L".cpp", L".h", L".hpp", L".c", L".cc", L".lua", L".cs", L".py"})) return Kind::Script;
    if (in({L".wav", L".mp3", L".ogg", L".flac"})) return Kind::Audio;
    if (in({L".hlsl", L".glsl", L".spv", L".fx", L".vert", L".frag"})) return Kind::Shader;
    return Kind::Other;
}

const wchar_t* KindTag(Kind k) {
    switch (k) {
    case Kind::Model: return L"3D";
    case Kind::Texture: return L"IMG";
    case Kind::Scene: return L"SCN";
    case Kind::Script: return L"SRC";
    case Kind::Audio: return L"SND";
    case Kind::Shader: return L"FX";
    default: return L"";
    }
}

COLORREF KindColor(Kind k) {
    switch (k) {
    case Kind::Model: return RGB(86, 140, 196);
    case Kind::Texture: return RGB(104, 168, 98);
    case Kind::Scene: return RGB(214, 150, 64);
    case Kind::Script: return RGB(168, 112, 190);
    case Kind::Audio: return RGB(200, 96, 110);
    case Kind::Shader: return RGB(70, 176, 170);
    default: return RGB(128, 120, 108);
    }
}

bool ContainsNoCase(const std::wstring& hay, const std::wstring& needle) {
    return Lower(hay).find(Lower(needle)) != std::wstring::npos;
}

void FillRectColor(HDC dc, const RECT& rc, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

void RoundRectColor(HDC dc, const RECT& rc, int radius, COLORREF fill, COLORREF line, bool outline) {
    HBRUSH b = CreateSolidBrush(fill);
    HPEN p = outline ? CreatePen(PS_SOLID, 1, line) : (HPEN)GetStockObject(NULL_PEN);
    HGDIOBJ oldB = SelectObject(dc, b);
    HGDIOBJ oldP = SelectObject(dc, p);
    RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(b);
    if (outline) DeleteObject(p);
}

// ------------------------------------------------------------------- files

void Collect(const std::wstring& filter, const std::wstring& dir, bool recursive, int depth, std::vector<Entry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(Join(dir, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
        Entry e;
        e.name = n;
        e.path = Join(dir, n);
        e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.kind = e.dir ? Kind::Folder : KindFor(e.name);
        e.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        if (filter.empty() || ContainsNoCase(e.name, filter)) out.push_back(e);
        if (e.dir && recursive && depth < 6) Collect(filter, e.path, true, depth + 1, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void CollectTree(const std::wstring& dir, int depth, std::vector<TreeNode>& out) {
    if (depth > 4) return;
    std::vector<Entry> kids;
    Collect(L"", dir, false, 0, kids);
    std::sort(kids.begin(), kids.end(), [](const Entry& a, const Entry& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    for (const Entry& e : kids)
        if (e.dir) {
            out.push_back({e.path, e.name, depth});
            CollectTree(e.path, depth + 1, out);
        }
}

bool FolderStamp(const std::wstring& dir, FILETIME& out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &d)) return false;
    out = d.ftLastWriteTime;
    return true;
}

void Reload(State& s, bool rebuildTree = true) {
    if (GetFileAttributesW(s.current.c_str()) == INVALID_FILE_ATTRIBUTES) s.current = s.root;
    s.entries.clear();
    Collect(s.filter, s.current, !s.filter.empty(), 0, s.entries);
    std::sort(s.entries.begin(), s.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    if (rebuildTree) {
        s.tree.clear();
        s.tree.push_back({s.root, L"Assets", 0});
        CollectTree(s.root, 1, s.tree);
    }
    if (s.selected >= (int)s.entries.size()) s.selected = -1;
    FolderStamp(s.current, s.stamp);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

bool IsCollapsed(const State& s, const std::wstring& path) {
    return std::find(s.collapsed.begin(), s.collapsed.end(), path) != s.collapsed.end();
}

bool HasChildren(const State& s, int index) {
    return index + 1 < (int)s.tree.size() && s.tree[index + 1].depth > s.tree[index].depth;
}

// Tree nodes that are on screen: everything except the children of collapsed folders (the tree is in depth-first order).
std::vector<int> VisibleTree(const State& s) {
    std::vector<int> out;
    int skipBelow = -1; // depth of the collapsed folder whose children are being skipped
    for (int i = 0; i < (int)s.tree.size(); ++i) {
        if (skipBelow >= 0) {
            if (s.tree[i].depth > skipBelow) continue;
            skipBelow = -1;
        }
        out.push_back(i);
        if (IsCollapsed(s, s.tree[i].path)) skipBelow = s.tree[i].depth;
    }
    return out;
}

void Navigate(State& s, const std::wstring& path, bool remember = true) {
    if (path == s.current) return;
    if (remember) s.history.push_back(s.current);
    s.current = path;
    s.selected = -1;
    s.scroll = 0;
    if (!s.filter.empty()) { // leaving the folder ends a search
        s.filter.clear();
        s.suppressSearch = true;
        SetWindowTextW(s.search, L"");
        s.suppressSearch = false;
    }
    Reload(s);
}

void GoUp(State& s) {
    if (s.current == s.root) return;
    const size_t cut = s.current.find_last_of(L'\\');
    if (cut == std::wstring::npos) return;
    std::wstring parent = s.current.substr(0, cut);
    if (parent.size() < s.root.size()) parent = s.root;
    Navigate(s, parent);
}

void GoBack(State& s) {
    if (s.history.empty()) return;
    const std::wstring target = s.history.back();
    s.history.pop_back();
    Navigate(s, target, false);
}

// SHFileOperation wants a double-null-terminated list.
std::wstring DoubleNull(const std::wstring& path) { return path + std::wstring(1, L'\0'); }

bool Delete(HWND hwnd, const std::wstring& path) {
    SHFILEOPSTRUCTW op = {};
    const std::wstring from = DoubleNull(path);
    op.hwnd = hwnd;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION; // recoverable: goes to the Recycle Bin
    return SHFileOperationW(&op) == 0;
}

void CopyInto(HWND hwnd, const std::wstring& from, const std::wstring& dir) {
    SHFILEOPSTRUCTW op = {};
    const std::wstring src = DoubleNull(from), dst = DoubleNull(dir);
    op.hwnd = hwnd;
    op.wFunc = FO_COPY;
    op.pFrom = src.c_str();
    op.pTo = dst.c_str();
    op.fFlags = FOF_NOCONFIRMMKDIR | FOF_RENAMEONCOLLISION;
    SHFileOperationW(&op);
}

// ---------------------------------------------------------------- geometry

int TabH(const State& s) { return S(s, 24); }    // strip with the drawer's title
int HeaderH(const State& s) { return TabH(s) + S(s, 32); } // title strip plus the navigation row
int TreeRowH(const State& s) { return S(s, 20); }
int FooterH(const State& s) { return S(s, 22); }
int SideW(const State& s, int w) { return w < S(s, 460) ? 0 : S(s, 170); }
int CellW(const State& s) { return S(s, s.tile + 18); }
int CellH(const State& s) { return S(s, s.tile + 34); }

RECT GridRect(const State& s, int w, int h) { return {SideW(s, w), HeaderH(s), w, std::max(HeaderH(s), h - FooterH(s))}; }

int Columns(const State& s, int w, int h) {
    const RECT g = GridRect(s, w, h);
    return std::max(1, (int)(g.right - g.left - S(s, 8)) / CellW(s));
}

int ContentHeight(const State& s, int w, int h) {
    const int cols = Columns(s, w, h);
    const int rows = ((int)s.entries.size() + cols - 1) / cols;
    return rows * CellH(s) + S(s, 16);
}

void ClampScroll(State& s, int w, int h) {
    const RECT g = GridRect(s, w, h);
    s.scroll = std::clamp(s.scroll, 0, std::max(0, ContentHeight(s, w, h) - (int)(g.bottom - g.top)));
}

RECT TileRect(const State& s, int index, int w, int h) {
    const RECT g = GridRect(s, w, h);
    const int cols = Columns(s, w, h);
    const int col = index % cols, row = index / cols;
    const int x = g.left + S(s, 8) + col * CellW(s);
    const int y = g.top + S(s, 8) + row * CellH(s) - s.scroll;
    return {x, y, x + CellW(s) - S(s, 4), y + CellH(s) - S(s, 4)};
}

int TileAt(const State& s, int x, int y, int w, int h) {
    const RECT g = GridRect(s, w, h);
    const POINT pt = {x, y};
    if (!PtInRect(&g, pt)) return -1;
    const int cols = Columns(s, w, h);
    const int col = (x - g.left - S(s, 8)) / CellW(s);
    const int row = (y - g.top - S(s, 8) + s.scroll) / CellH(s);
    if (x < g.left + S(s, 8) || y < g.top + S(s, 8) - s.scroll || col >= cols) return -1;
    const int index = row * cols + col;
    if (index < 0 || index >= (int)s.entries.size()) return -1;
    const RECT rc = TileRect(s, index, w, h);
    return PtInRect(&rc, pt) ? index : -1;
}

RECT BackRect(const State& s) { return {S(s, 8), TabH(s) + S(s, 4), S(s, 32), TabH(s) + S(s, 28)}; }
RECT UpRect(const State& s) { return {S(s, 36), TabH(s) + S(s, 4), S(s, 60), TabH(s) + S(s, 28)}; }
int SearchW(const State& s) { return S(s, 190); }
// Framed search field at the right end of the navigation row (the edit control sits inside it).
RECT SearchFrame(const State& s, int w) { return {w - SearchW(s) - S(s, 8), TabH(s) + S(s, 4), w - S(s, 8), TabH(s) + S(s, 28)}; }

std::vector<Crumb> BuildCrumbs(State& s, HDC dc, int w) {
    std::vector<Crumb> crumbs;
    std::vector<std::pair<std::wstring, std::wstring>> parts; // label, absolute path
    parts.push_back({L"Assets", s.root});
    if (s.current.size() > s.root.size()) {
        std::wstring rel = s.current.substr(s.root.size());
        std::wstring acc = s.root;
        size_t i = 0;
        while (i < rel.size()) {
            while (i < rel.size() && rel[i] == L'\\') ++i;
            size_t end = rel.find(L'\\', i);
            if (end == std::wstring::npos) end = rel.size();
            if (end > i) {
                const std::wstring name = rel.substr(i, end - i);
                acc = Join(acc, name);
                parts.push_back({name, acc});
            }
            i = end;
        }
    }
    int x = S(s, 72);
    const int limit = (int)SearchFrame(s, w).left - S(s, 12);
    HGDIOBJ old = SelectObject(dc, s.bold);
    for (size_t i = 0; i < parts.size(); ++i) {
        SIZE sz;
        GetTextExtentPoint32W(dc, parts[i].first.c_str(), (int)parts[i].first.size(), &sz);
        if (x + sz.cx > limit) break;
        crumbs.push_back({{x, TabH(s) + S(s, 4), x + sz.cx + S(s, 8), TabH(s) + S(s, 28)}, parts[i].second, parts[i].first});
        x += sz.cx + S(s, 8) + S(s, 14);
    }
    SelectObject(dc, old);
    return crumbs;
}

// ------------------------------------------------------------------ painting

void DrawIcon(HDC dc, const State& s, const RECT& box, const Entry& e) {
    const int bw = box.right - box.left, bh = box.bottom - box.top;
    if (e.dir) {
        const int pad = bw / 8;
        const RECT tab = {box.left + pad, box.top + bh / 5, box.left + pad + bw / 3, box.top + bh / 5 + bh / 8};
        const RECT body = {box.left + pad, box.top + bh / 5 + bh / 12, box.right - pad, box.bottom - bh / 6};
        RoundRectColor(dc, tab, S(s, 4), RGB(196, 148, 58), 0, false);
        RoundRectColor(dc, body, S(s, 6), RGB(224, 176, 78), 0, false);
        const RECT shade = {body.left, body.top + (body.bottom - body.top) / 3, body.right, body.bottom};
        RoundRectColor(dc, shade, S(s, 6), RGB(236, 192, 98), 0, false);
        return;
    }
    const int padX = bw / 5, padY = bh / 10;
    const RECT page = {box.left + padX, box.top + padY, box.right - padX, box.bottom - padY};
    const COLORREF c = KindColor(e.kind);
    RoundRectColor(dc, page, S(s, 4), theme::kPanel, c, true);
    const RECT band = {page.left + S(s, 3), page.top + (page.bottom - page.top) * 2 / 3, page.right - S(s, 3),
                       page.bottom - S(s, 3)};
    RoundRectColor(dc, band, S(s, 3), c, 0, false);
    std::wstring tag = KindTag(e.kind);
    if (tag.empty()) { // unknown type: show its extension
        tag = Extension(e.name);
        if (!tag.empty()) tag.erase(0, 1);
        for (auto& ch : tag) ch = (wchar_t)towupper(ch);
        if (tag.size() > 4) tag.resize(4);
    }
    SetTextColor(dc, RGB(255, 255, 255));
    HGDIOBJ old = SelectObject(dc, s.bold);
    RECT t = band;
    DrawTextW(dc, tag.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(dc, old);
    // Page glyph above the band: a few "text" lines, or a mountain for images.
    HPEN pen = CreatePen(PS_SOLID, std::max(1, S(s, 2)), c);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    const int gx0 = page.left + S(s, 10), gx1 = page.right - S(s, 10);
    const int gy0 = page.top + S(s, 10), gy1 = band.top - S(s, 8);
    if (e.kind == Kind::Texture) {
        POINT m[3] = {{gx0, gy1}, {(gx0 + gx1) / 2, gy0}, {gx1, gy1}};
        HBRUSH b = CreateSolidBrush(c);
        HGDIOBJ oldB = SelectObject(dc, b);
        Polygon(dc, m, 3);
        SelectObject(dc, oldB);
        DeleteObject(b);
    } else {
        for (int y = gy0; y < gy1; y += S(s, 7)) {
            MoveToEx(dc, gx0, y, nullptr);
            LineTo(dc, gx1 - ((y / S(s, 7)) % 3) * S(s, 6), y);
        }
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

std::wstring SizeText(ULONGLONG bytes) {
    wchar_t buf[48];
    if (bytes < 1024) swprintf(buf, 48, L"%llu B", bytes);
    else if (bytes < 1024 * 1024) swprintf(buf, 48, L"%.1f KB", (double)bytes / 1024.0);
    else swprintf(buf, 48, L"%.1f MB", (double)bytes / (1024.0 * 1024.0));
    return buf;
}

void DrawArrow(HDC dc, const State& s, const RECT& rc, bool up, bool enabled, bool hot) {
    if (hot && enabled) FillRectColor(dc, rc, kHover);
    const COLORREF c = enabled ? kText : theme::kDisabled;
    HPEN pen = CreatePen(PS_SOLID, std::max(2, S(s, 2)), c);
    HGDIOBJ old = SelectObject(dc, pen);
    const int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2, r = S(s, 5);
    if (up) {
        MoveToEx(dc, cx - r, cy + r / 2, nullptr);
        LineTo(dc, cx, cy - r / 2);
        LineTo(dc, cx + r, cy + r / 2);
    } else {
        MoveToEx(dc, cx + r / 2, cy - r, nullptr);
        LineTo(dc, cx - r / 2, cy);
        LineTo(dc, cx + r / 2, cy + r);
    }
    SelectObject(dc, old);
    DeleteObject(pen);
}

void Paint(HWND hwnd, State& s) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right, h = client.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldFont = SelectObject(mem, s.font);
    SetBkMode(mem, TRANSPARENT);
    FillRectColor(mem, client, kBg);

    ClampScroll(s, w, h);
    const RECT grid = GridRect(s, w, h);
    POINT cursor;
    GetCursorPos(&cursor);
    ScreenToClient(hwnd, &cursor);

    // Tiles, clipped to the grid area.
    const int saved = SaveDC(mem);
    IntersectClipRect(mem, grid.left, grid.top, grid.right, grid.bottom);
    for (int i = 0; i < (int)s.entries.size(); ++i) {
        const RECT rc = TileRect(s, i, w, h);
        if (rc.bottom < grid.top || rc.top > grid.bottom) continue;
        const Entry& e = s.entries[i];
        const bool sel = i == s.selected, hot = i == s.hover;
        if (sel || hot) FillRectColor(mem, rc, sel ? theme::kRaised : theme::kPanel);
        if (sel) theme::Frame(mem, rc, kAccent);
        const int iconSize = S(s, s.tile);
        const RECT icon = {(rc.left + rc.right - iconSize) / 2, rc.top + S(s, 2), (rc.left + rc.right + iconSize) / 2,
                           rc.top + S(s, 2) + iconSize};
        DrawIcon(mem, s, icon, e);
        if (i != s.renaming) {
            if (sel) FillRectColor(mem, {rc.left + 1, icon.bottom + S(s, 1), rc.right - 1, rc.bottom - 1}, kAccent);
            SetTextColor(mem, sel ? RGB(255, 255, 255) : kText);
            RECT label = {rc.left + S(s, 3), icon.bottom + S(s, 2), rc.right - S(s, 3), rc.bottom};
            DrawTextW(mem, e.name.c_str(), -1, &label, DT_CENTER | DT_END_ELLIPSIS | DT_NOPREFIX | DT_WORDBREAK);
        }
    }
    if (s.entries.empty()) {
        SetTextColor(mem, kMuted);
        RECT msg = grid;
        const wchar_t* text = s.filter.empty() ? L"This folder is empty.\nRight-click to create a folder, or drag files here from Explorer."
                                               : L"Nothing matches your search.";
        DrawTextW(mem, text, -1, &msg, DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    }
    RestoreDC(mem, saved);

    // Folder tree.
    const int sideW = SideW(s, w);
    if (sideW > 0) {
        const RECT side = {0, HeaderH(s), sideW, std::max(HeaderH(s), h - FooterH(s))};
        FillRectColor(mem, side, kSideBg);
        FillRectColor(mem, {sideW - 1, side.top, sideW, side.bottom}, theme::kLine);
        const int saved2 = SaveDC(mem);
        IntersectClipRect(mem, side.left, side.top, side.right, side.bottom);
        int y = side.top + S(s, 4);
        for (int index : VisibleTree(s)) {
            const TreeNode& n = s.tree[index];
            const bool current = n.path == s.current;
            const RECT row = {0, y, sideW - 1, y + TreeRowH(s)};
            if (current) FillRectColor(mem, row, kAccent);
            else if (PtInRect(&row, cursor)) FillRectColor(mem, row, kHover);
            const int arrowX = S(s, 12) + n.depth * S(s, 12), midY = y + TreeRowH(s) / 2;
            if (HasChildren(s, index))
                theme::DrawDisclosure(mem, arrowX, midY, S(s, 4), !IsCollapsed(s, n.path), current ? RGB(255, 255, 255) : kMuted);
            // Little folder glyph, then the name.
            const int gx = arrowX + S(s, 9), gy = midY - S(s, 4);
            FillRectColor(mem, {gx, gy, gx + S(s, 5), gy + S(s, 2) + 1}, RGB(196, 148, 58));
            FillRectColor(mem, {gx, gy + S(s, 2), gx + S(s, 12), gy + S(s, 9)}, RGB(224, 176, 78));
            SetTextColor(mem, current ? RGB(255, 255, 255) : kText);
            RECT t = {gx + S(s, 17), row.top, row.right - S(s, 4), row.bottom};
            DrawTextW(mem, n.name.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
            y += TreeRowH(s);
        }
        RestoreDC(mem, saved2);
    }

    // Tab strip with the drawer's title, then the navigation row (back, up, path, search).
    FillRectColor(mem, {0, 0, w, TabH(s)}, kHeaderBg);
    FillRectColor(mem, {S(s, 6), S(s, 4), S(s, 6) + S(s, 124), TabH(s)}, theme::kPanel);
    SelectObject(mem, s.small);
    SetTextColor(mem, theme::kTextBright);
    theme::DrawCaps(mem, {S(s, 16), S(s, 4), S(s, 128), TabH(s)}, L"CONTENT DRAWER");
    FillRectColor(mem, {0, TabH(s), w, HeaderH(s)}, theme::kPanel);
    FillRectColor(mem, {0, HeaderH(s) - 1, w, HeaderH(s)}, theme::kLine);
    {
        const RECT f = SearchFrame(s, w);
        FillRectColor(mem, f, theme::kWindow);
        theme::Frame(mem, f, GetFocus() == s.search ? kAccent : theme::kRule);
        theme::DrawMagnifier(mem, f.left + S(s, 7), f.top + S(s, 7), S(s, 8), std::max(1, S(s, 1)), kMuted);
    }
    SelectObject(mem, s.font);
    const RECT back = BackRect(s), up = UpRect(s);
    DrawArrow(mem, s, back, false, !s.history.empty(), PtInRect(&back, cursor));
    DrawArrow(mem, s, up, true, s.current != s.root, PtInRect(&up, cursor));
    const std::vector<Crumb> crumbs = BuildCrumbs(s, mem, w);
    SelectObject(mem, s.bold);
    for (size_t i = 0; i < crumbs.size(); ++i) {
        const bool last = i + 1 == crumbs.size();
        if (PtInRect(&crumbs[i].rc, cursor) && !last) FillRectColor(mem, crumbs[i].rc, kHover);
        SetTextColor(mem, last ? kText : kMuted);
        RECT t = crumbs[i].rc;
        DrawTextW(mem, crumbs[i].label.c_str(), -1, &t, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        if (!last) {
            SetTextColor(mem, theme::kDisabled);
            RECT sep = {crumbs[i].rc.right, crumbs[i].rc.top, crumbs[i].rc.right + S(s, 14), crumbs[i].rc.bottom};
            DrawTextW(mem, L"\x203A", -1, &sep, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
        }
    }
    SelectObject(mem, s.font);

    // Footer.
    FillRectColor(mem, {0, h - FooterH(s), w, h}, kHeaderBg);
    FillRectColor(mem, {0, h - FooterH(s), w, h - FooterH(s) + 1}, kBorder);
    wchar_t status[600];
    if (s.selected >= 0 && s.selected < (int)s.entries.size()) {
        const Entry& e = s.entries[s.selected];
        swprintf(status, 600, L"%zu items  |  %ls%ls", s.entries.size(), e.name.c_str(),
                 e.dir ? L"" : (L"  (" + SizeText(e.size) + L")").c_str());
    } else {
        swprintf(status, 600, L"%zu items", s.entries.size());
    }
    SetTextColor(mem, kMuted);
    SelectObject(mem, s.small);
    RECT st = {S(s, 10), h - FooterH(s), w - S(s, 10), h};
    DrawTextW(mem, status, -1, &st, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
    const wchar_t* hint = L"Ctrl+Wheel: tile size   F2: rename   Del: delete";
    DrawTextW(mem, hint, -1, &st, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    // Scrollbar thumb.
    const int total = ContentHeight(s, w, h), view = grid.bottom - grid.top;
    if (total > view) {
        const int trackH = view - S(s, 8);
        const int thumbH = std::max(S(s, 24), trackH * view / total);
        const int maxScroll = total - view;
        const int thumbY = grid.top + S(s, 4) + (trackH - thumbH) * s.scroll / std::max(1, maxScroll);
        FillRectColor(mem, {w - S(s, 7), thumbY, w - S(s, 3), thumbY + thumbH}, theme::kHover);
    }

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// ---------------------------------------------------------------- operations

void Layout(State& s) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT f = SearchFrame(s, rc.right);
    MoveWindow(s.search, f.left + S(s, 24), f.top + S(s, 4), std::max(10, (int)(f.right - f.left) - S(s, 30)), S(s, 17), TRUE);
    ClampScroll(s, rc.right, rc.bottom);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void OpenEntry(State& s, int index) {
    if (index < 0 || index >= (int)s.entries.size()) return;
    const Entry e = s.entries[index];
    if (e.dir) Navigate(s, e.path);
    else if (s.cb.openAsset) s.cb.openAsset(e.path, s.cb.user);
}

void EndRename(State& s, bool commit) {
    if (s.renaming < 0) return;
    const int index = s.renaming;
    s.renaming = -1; // first: hiding the edit sends a kill-focus that would re-enter
    wchar_t buf[260] = {};
    GetWindowTextW(s.rename, buf, 260);
    ShowWindow(s.rename, SW_HIDE);
    SetFocus(s.hwnd);
    std::wstring name = buf;
    if (commit && index < (int)s.entries.size() && !name.empty() && name != s.entries[index].name &&
        name.find_first_of(L"\\/:*?\"<>|") == std::wstring::npos) {
        const std::wstring from = s.entries[index].path;
        const std::wstring to = Join(from.substr(0, from.find_last_of(L'\\')), name);
        if (MoveFileW(from.c_str(), to.c_str())) {
            Reload(s);
            for (int i = 0; i < (int)s.entries.size(); ++i)
                if (s.entries[i].path == to) s.selected = i;
        }
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void BeginRename(State& s, int index) {
    if (index < 0 || index >= (int)s.entries.size()) return;
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const RECT tile = TileRect(s, index, rc.right, rc.bottom);
    const int labelTop = tile.top + S(s, 2) + S(s, s.tile) + S(s, 2);
    s.renaming = index;
    SetWindowTextW(s.rename, s.entries[index].name.c_str());
    MoveWindow(s.rename, tile.left, labelTop, tile.right - tile.left, S(s, 20), TRUE);
    ShowWindow(s.rename, SW_SHOW);
    SetFocus(s.rename);
    // Select the name without its extension, like Explorer.
    const size_t dot = s.entries[index].dir ? std::wstring::npos : s.entries[index].name.rfind(L'.');
    SendMessageW(s.rename, EM_SETSEL, 0, dot == std::wstring::npos ? -1 : (LPARAM)dot);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

void NewFolder(State& s) {
    std::wstring path;
    for (int i = 0;; ++i) {
        const std::wstring name = i == 0 ? L"New Folder" : L"New Folder (" + std::to_wstring(i + 1) + L")";
        path = Join(s.current, name);
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    }
    if (!CreateDirectoryW(path.c_str(), nullptr)) return;
    Reload(s);
    for (int i = 0; i < (int)s.entries.size(); ++i)
        if (s.entries[i].path == path) {
            s.selected = i;
            RECT rc;
            GetClientRect(s.hwnd, &rc);
            const RECT t = TileRect(s, i, rc.right, rc.bottom); // scroll the new folder into view
            const RECT g = GridRect(s, rc.right, rc.bottom);
            if (t.bottom > g.bottom) s.scroll += t.bottom - g.bottom + S(s, 8);
            BeginRename(s, i);
            break;
        }
}

void DeleteSelected(State& s) {
    if (s.selected < 0 || s.selected >= (int)s.entries.size()) return;
    const Entry e = s.entries[s.selected];
    const std::wstring msg = L"Move \"" + e.name + L"\" to the Recycle Bin?";
    if (MessageBoxW(s.hwnd, msg.c_str(), L"Delete", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    Delete(s.hwnd, e.path);
    s.selected = -1;
    Reload(s);
}

void ShowInExplorer(State& s) {
    if (s.selected >= 0 && s.selected < (int)s.entries.size()) {
        const std::wstring arg = L"/select,\"" + s.entries[s.selected].path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOW);
    } else {
        ShellExecuteW(nullptr, L"open", s.current.c_str(), nullptr, nullptr, SW_SHOW);
    }
}

void ContextMenu(State& s, POINT screen) {
    enum { kOpen = 1, kNewFolder, kRename, kDelete, kExplorer, kRefresh };
    const bool has = s.selected >= 0 && s.selected < (int)s.entries.size();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (has ? 0 : MF_GRAYED), kOpen, L"Open");
    AppendMenuW(menu, MF_STRING | (has ? 0 : MF_GRAYED), kRename, L"Rename\tF2");
    AppendMenuW(menu, MF_STRING | (has ? 0 : MF_GRAYED), kDelete, L"Delete\tDel");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kNewFolder, L"New Folder");
    AppendMenuW(menu, MF_STRING, kExplorer, L"Show in Explorer");
    AppendMenuW(menu, MF_STRING, kRefresh, L"Refresh\tF5");
    const int cmd = theme::TrackMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, screen.x, screen.y, s.hwnd, s.dpi);
    DestroyMenu(menu);
    switch (cmd) {
    case kOpen: OpenEntry(s, s.selected); break;
    case kRename: BeginRename(s, s.selected); break;
    case kDelete: DeleteSelected(s); break;
    case kNewFolder: NewFolder(s); break;
    case kExplorer: ShowInExplorer(s); break;
    case kRefresh: Reload(s); break;
    }
}

void OnLeftDown(State& s, int x, int y, bool doubleClick) {
    RECT rc;
    GetClientRect(s.hwnd, &rc);
    const int w = rc.right, h = rc.bottom;
    SetFocus(s.hwnd);
    const POINT pt = {x, y};

    if (y < HeaderH(s)) {
        const RECT back = BackRect(s), up = UpRect(s);
        if (PtInRect(&back, pt)) { GoBack(s); return; }
        if (PtInRect(&up, pt)) { GoUp(s); return; }
        HDC dc = GetDC(s.hwnd);
        const std::vector<Crumb> crumbs = BuildCrumbs(s, dc, w);
        ReleaseDC(s.hwnd, dc);
        for (const Crumb& c : crumbs)
            if (PtInRect(&c.rc, pt)) { Navigate(s, c.path); return; }
        return;
    }

    const int sideW = SideW(s, w);
    if (x < sideW) { // folder tree: the arrow folds a folder, the rest opens it
        const std::vector<int> visible = VisibleTree(s);
        const int top = HeaderH(s) + S(s, 4);
        const int row = y >= top ? (y - top) / TreeRowH(s) : -1;
        if (row >= 0 && row < (int)visible.size()) {
            const int index = visible[row];
            const std::wstring path = s.tree[index].path;
            const int arrowX = S(s, 12) + s.tree[index].depth * S(s, 12);
            if (HasChildren(s, index) && x >= arrowX - S(s, 7) && x <= arrowX + S(s, 7)) {
                auto it = std::find(s.collapsed.begin(), s.collapsed.end(), path);
                if (it == s.collapsed.end()) s.collapsed.push_back(path);
                else s.collapsed.erase(it);
                InvalidateRect(s.hwnd, nullptr, FALSE);
            } else {
                Navigate(s, path);
            }
        }
        return;
    }

    const int index = TileAt(s, x, y, w, h);
    s.selected = index;
    if (doubleClick && index >= 0) OpenEntry(s, index);
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK RenameProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    State* s = reinterpret_cast<State*>(ref);
    if (msg == WM_KEYDOWN && (wparam == VK_RETURN || wparam == VK_ESCAPE)) {
        PostMessageW(s->hwnd, kMsgEndRename, wparam == VK_RETURN ? 1 : 0, 0);
        return 0;
    }
    if (msg == WM_CHAR && (wparam == VK_RETURN || wparam == VK_ESCAPE)) return 0; // no beep
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK SearchProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    State* s = reinterpret_cast<State*>(ref);
    if (msg == WM_KEYDOWN && wparam == VK_ESCAPE) {
        SetWindowTextW(hwnd, L"");
        SetFocus(s->hwnd);
        return 0;
    }
    if (msg == WM_CHAR && wparam == VK_ESCAPE) return 0;
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK BrowserProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        break;
    case WM_ERASEBKGND:
        return 1;
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
        if (s && wparam == kWatchTimer) { // pick up files added or removed outside the editor
            FILETIME now;
            if (s->renaming < 0 && FolderStamp(s->current, now) && CompareFileTime(&now, &s->stamp) != 0) Reload(*s);
        }
        return 0;
    case WM_COMMAND:
        if (s && (HWND)lparam == s->search && HIWORD(wparam) == EN_CHANGE && !s->suppressSearch) {
            wchar_t buf[128] = {};
            GetWindowTextW(s->search, buf, 128);
            s->filter = buf;
            s->selected = -1;
            s->scroll = 0;
            Reload(*s, false);
        } else if (s && (HWND)lparam == s->rename && HIWORD(wparam) == EN_KILLFOCUS) {
            EndRename(*s, true);
        } else if (s && (HWND)lparam == s->search && (HIWORD(wparam) == EN_SETFOCUS || HIWORD(wparam) == EN_KILLFOCUS)) {
            InvalidateRect(hwnd, nullptr, FALSE); // the search frame turns blue while typing
        }
        return 0;
    case kMsgEndRename:
        if (s) EndRename(*s, wparam != 0);
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = (HDC)wparam;
        SetTextColor(dc, kText);
        SetBkColor(dc, theme::kWindow);
        return s ? (LRESULT)s->editBrush : 0;
    }
    case WM_LBUTTONDOWN:
        if (s) OnLeftDown(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), false);
        return 0;
    case WM_LBUTTONDBLCLK:
        if (s) OnLeftDown(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), true);
        return 0;
    case WM_RBUTTONDOWN:
        if (s) {
            SetFocus(hwnd);
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int hit = TileAt(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), rc.right, rc.bottom);
            if (hit >= 0 || GET_X_LPARAM(lparam) >= SideW(*s, rc.right)) s->selected = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ClientToScreen(hwnd, &pt);
            ContextMenu(*s, pt);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (s) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int hover = TileAt(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam), rc.right, rc.bottom);
            if (hover != s->hover) {
                s->hover = hover;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (GET_Y_LPARAM(lparam) < HeaderH(*s) || GET_X_LPARAM(lparam) < SideW(*s, rc.right))
                InvalidateRect(hwnd, nullptr, FALSE); // header and folder tree hover
            if (!s->tracking) {
                TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
                s->tracking = TrackMouseEvent(&t) != FALSE;
            }
        }
        return 0;
    case WM_MOUSELEAVE:
        if (s) {
            s->tracking = false;
            s->hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (s) {
            const int notches = GET_WHEEL_DELTA_WPARAM(wparam);
            if (GET_KEYSTATE_WPARAM(wparam) & MK_CONTROL) {
                s->tile = std::clamp(s->tile + notches / WHEEL_DELTA * 12, 56, 180);
            } else {
                s->scroll -= notches * S(*s, 60) / WHEEL_DELTA;
            }
            RECT rc;
            GetClientRect(hwnd, &rc);
            ClampScroll(*s, rc.right, rc.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_KEYDOWN:
        if (s) {
            switch (wparam) {
            case VK_F5: Reload(*s); return 0;
            case VK_F2: BeginRename(*s, s->selected); return 0;
            case VK_DELETE: DeleteSelected(*s); return 0;
            case VK_BACK: GoUp(*s); return 0;
            case VK_RETURN: OpenEntry(*s, s->selected); return 0;
            case VK_LEFT:
            case VK_RIGHT:
            case VK_UP:
            case VK_DOWN: {
                if (s->entries.empty()) return 0;
                RECT rc;
                GetClientRect(hwnd, &rc);
                const int cols = Columns(*s, rc.right, rc.bottom);
                int i = s->selected < 0 ? 0 : s->selected;
                if (s->selected >= 0) {
                    if (wparam == VK_LEFT) i -= 1;
                    if (wparam == VK_RIGHT) i += 1;
                    if (wparam == VK_UP) i -= cols;
                    if (wparam == VK_DOWN) i += cols;
                }
                s->selected = std::clamp(i, 0, (int)s->entries.size() - 1);
                const RECT t = TileRect(*s, s->selected, rc.right, rc.bottom);
                const RECT g = GridRect(*s, rc.right, rc.bottom);
                if (t.top < g.top) s->scroll -= g.top - t.top + S(*s, 8);
                if (t.bottom > g.bottom) s->scroll += t.bottom - g.bottom + S(*s, 8);
                ClampScroll(*s, rc.right, rc.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            }
        }
        break;
    case WM_DROPFILES:
        if (s) {
            HDROP drop = (HDROP)wparam;
            const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t path[MAX_PATH] = {};
                if (DragQueryFileW(drop, i, path, MAX_PATH)) CopyInto(hwnd, path, s->current);
            }
            DragFinish(drop);
            Reload(*s);
        }
        return 0;
    case WM_SETCURSOR:
        break;
    case WM_NCDESTROY:
        if (s) {
            KillTimer(hwnd, kWatchTimer);
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

void EnsureDirectory(const std::wstring& path) {
    // Creates every missing level of an absolute path.
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

} // namespace

int ContentBrowser_DefaultHeight(int dpi) { return MulDiv(250, dpi, 96); }
int ContentBrowser_MinHeight(int dpi) { return MulDiv(120, dpi, 96); }

HWND ContentBrowser_Create(HWND parent, HINSTANCE instance, int dpi, const std::wstring& root,
                           const ContentBrowserCallbacks& callbacks) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = BrowserProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBrowserClass;
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }

    auto* s = new State;
    s->dpi = dpi;
    s->cb = callbacks;
    s->root = root;
    while (s->root.size() > 3 && s->root.back() == L'\\') s->root.pop_back();
    EnsureDirectory(s->root);
    s->current = s->root;
    s->editBrush = CreateSolidBrush(theme::kWindow);
    auto makeFont = [&](int px, int weight) {
        return CreateFontW(-S(*s, px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    s->font = makeFont(13, FW_NORMAL);
    s->small = makeFont(11, FW_NORMAL);
    s->bold = makeFont(13, FW_SEMIBOLD);

    HWND hwnd = CreateWindowExW(0, kBrowserClass, L"Content Browser", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0,
                                100, ContentBrowser_DefaultHeight(dpi), parent, nullptr, instance, s);
    if (!hwnd) {
        DeleteObject(s->font);
        DeleteObject(s->small);
        DeleteObject(s->bold);
        DeleteObject(s->editBrush);
        delete s;
        return nullptr;
    }
    s->hwnd = hwnd;

    s->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 100, 20, hwnd, nullptr,
                                instance, nullptr);
    SendMessageW(s->search, WM_SETFONT, (WPARAM)s->font, TRUE);
    SendMessageW(s->search, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search assets");
    SetWindowSubclass(s->search, SearchProc, 1, (DWORD_PTR)s);

    s->rename = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_CENTER, 0, 0, 100, 20, hwnd,
                                nullptr, instance, nullptr);
    SendMessageW(s->rename, WM_SETFONT, (WPARAM)s->font, TRUE);
    SetWindowSubclass(s->rename, RenameProc, 2, (DWORD_PTR)s);

    DragAcceptFiles(hwnd, TRUE);
    SetTimer(hwnd, kWatchTimer, 1000, nullptr);
    Reload(*s);
    Layout(*s);
    return hwnd;
}

std::wstring ContentBrowser_CurrentFolder(HWND browser) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(browser, GWLP_USERDATA));
    return s ? s->current : std::wstring();
}

void ContentBrowser_Refresh(HWND browser) {
    if (auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(browser, GWLP_USERDATA))) Reload(*s);
}

} // namespace tbd
