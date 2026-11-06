// Shared look of the Win32 editor panels (toolbar, outliner, details, content drawer): dark charcoal
// surfaces, thin separators, a single blue accent for selection, compact rows. The palette and the
// little shapes below are this engine's own; nothing is taken from another product.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace tbd {
namespace theme {

inline const COLORREF kWindow = RGB(0x1a, 0x1a, 0x1a);  // gaps between panels, tab strips, text fields
inline const COLORREF kPanel = RGB(0x24, 0x24, 0x24);   // panel body
inline const COLORREF kStripe = RGB(0x27, 0x27, 0x27);  // every other list row
inline const COLORREF kRaised = RGB(0x30, 0x30, 0x30);  // toggled-on and pressed-in flat buttons
inline const COLORREF kHover = RGB(0x3a, 0x3a, 0x3a);   // flat button / row under the cursor
inline const COLORREF kLine = RGB(0x0f, 0x0f, 0x0f);    // panel edges
inline const COLORREF kRule = RGB(0x3a, 0x3a, 0x3a);    // thin separators inside a panel
inline const COLORREF kText = RGB(0xcc, 0xcc, 0xcc);
inline const COLORREF kTextBright = RGB(0xf2, 0xf2, 0xf2);
inline const COLORREF kMuted = RGB(0x8c, 0x8c, 0x8c);
inline const COLORREF kDisabled = RGB(0x5c, 0x5c, 0x5c);
inline const COLORREF kAccent = RGB(0x00, 0x70, 0xe0);  // the one accent: selection, active tool, focus

inline void Fill(HDC dc, const RECT& rc, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FillRect(dc, &rc, b);
    DeleteObject(b);
}

inline void Frame(HDC dc, const RECT& rc, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    FrameRect(dc, &rc, b);
    DeleteObject(b);
}

inline HFONT MakeFont(int dpi, int px, int weight) {
    return CreateFontW(-MulDiv(px, dpi, 96), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

inline void FillPolygon(HDC dc, const POINT* pts, int count, COLORREF color) {
    HBRUSH b = CreateSolidBrush(color);
    HGDIOBJ oldB = SelectObject(dc, b);
    HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    Polygon(dc, pts, count);
    SelectObject(dc, oldP);
    SelectObject(dc, oldB);
    DeleteObject(b);
}

// Collapse arrow centred on (cx, cy): pointing right when closed, down when open. r is its half size.
inline void DrawDisclosure(HDC dc, int cx, int cy, int r, bool open, COLORREF color) {
    POINT pts[3];
    if (open) {
        pts[0] = {cx - r, cy - r / 2};
        pts[1] = {cx + r, cy - r / 2};
        pts[2] = {cx, cy + r / 2 + 1};
    } else {
        pts[0] = {cx - r / 2, cy - r};
        pts[1] = {cx - r / 2, cy + r};
        pts[2] = {cx + r / 2 + 1, cy};
    }
    FillPolygon(dc, pts, 3, color);
}

// Search glyph (a ring with a handle) whose ring is d pixels wide, top-left at (x, y).
inline void DrawMagnifier(HDC dc, int x, int y, int d, int penWidth, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, penWidth, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Ellipse(dc, x, y, x + d, y + d);
    MoveToEx(dc, x + d - 1, y + d - 1, nullptr);
    LineTo(dc, x + d + d / 3 + 1, y + d + d / 3 + 1);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

// Small-caps style section header: text is expected already upper case; a little letter spacing does the rest.
inline void DrawCaps(HDC dc, RECT rc, const wchar_t* text, UINT flags = DT_SINGLELINE | DT_VCENTER) {
    SetTextCharacterExtra(dc, 1);
    DrawTextW(dc, text, -1, &rc, flags | DT_NOPREFIX);
    SetTextCharacterExtra(dc, 0);
}

// ------------------------------------------------------------------ dark popup menus
//
// Win32 popup menus are drawn by the system in light colours. TrackMenu() below shows a menu with every item
// switched to owner-draw for the duration of the call (and switched back afterwards, so a menu that lives
// longer than one popup, like the File menu, keeps its plain strings). The window that owns the popup must
// forward WM_MEASUREITEM and WM_DRAWITEM to HandleMenuMessage():
//
//     case WM_MEASUREITEM: case WM_DRAWITEM: { LRESULT r = 0; if (theme::HandleMenuMessage(msg, lparam, &r)) return r; break; }

namespace menu_detail {

constexpr unsigned kMagic = 0x7EEDB0A1u;

struct Item {
    unsigned magic = kMagic;
    std::wstring text, accel; // "Frame\tF" is split at the tab
    bool separator = false, radio = false, popup = false;
    int width = 0, height = 0; // device pixels, as measured when the menu was prepared
};

struct Saved {
    HMENU menu = nullptr;
    UINT pos = 0;
    std::wstring raw;
    bool separator = false, radio = false;
};

struct Ctx {
    int dpi = 96;
    HFONT font = nullptr;
};

inline Ctx& Context() {
    static Ctx c;
    return c;
}

inline int Px(int v) { return MulDiv(v, Context().dpi, 96); }

inline std::wstring ItemText(HMENU menu, UINT pos) {
    MENUITEMINFOW mi = {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_STRING;
    mi.dwTypeData = nullptr;
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mi) || mi.cch == 0) return L"";
    std::wstring text((size_t)mi.cch + 1, L'\0');
    mi.fMask = MIIM_STRING;
    mi.dwTypeData = &text[0];
    mi.cch = (UINT)text.size();
    if (!GetMenuItemInfoW(menu, pos, TRUE, &mi)) return L"";
    text.resize(mi.cch);
    return text;
}

inline int TextWidth(HDC dc, const std::wstring& t) {
    if (t.empty()) return 0;
    SIZE sz = {};
    GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
    return sz.cx;
}

// Switches every item of `menu` (and its submenus) to owner-draw; `saved` remembers how to undo it.
inline void Prepare(HMENU menu, HDC dc, std::vector<std::unique_ptr<Item>>& items, std::vector<Saved>& saved) {
    const int n = GetMenuItemCount(menu);
    for (int i = 0; i < n; ++i) {
        MENUITEMINFOW mi = {};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_SUBMENU;
        if (!GetMenuItemInfoW(menu, (UINT)i, TRUE, &mi)) continue;
        if (mi.fType & MFT_OWNERDRAW) continue;
        auto item = std::make_unique<Item>();
        item->separator = (mi.fType & MFT_SEPARATOR) != 0;
        item->radio = (mi.fType & MFT_RADIOCHECK) != 0;
        item->popup = mi.hSubMenu != nullptr;
        Saved sv;
        sv.menu = menu;
        sv.pos = (UINT)i;
        sv.separator = item->separator;
        sv.radio = item->radio;
        if (!item->separator) {
            sv.raw = ItemText(menu, (UINT)i);
            const size_t tab = sv.raw.find(L'\t');
            item->text = sv.raw.substr(0, tab);
            if (tab != std::wstring::npos) item->accel = sv.raw.substr(tab + 1);
        }
        item->height = Px(item->separator ? 7 : 24);
        item->width = item->separator
                          ? Px(60)
                          : Px(30) + TextWidth(dc, item->text) + (item->accel.empty() ? 0 : Px(28) + TextWidth(dc, item->accel)) +
                                Px(item->popup ? 28 : 18);
        MENUITEMINFOW set = {};
        set.cbSize = sizeof(set);
        set.fMask = MIIM_FTYPE | MIIM_DATA | (item->separator ? MIIM_STATE : 0);
        set.fType = MFT_OWNERDRAW | (item->radio ? MFT_RADIOCHECK : 0);
        set.fState = MFS_DISABLED; // only used for separators: keeps them from being picked
        set.dwItemData = (ULONG_PTR)item.get();
        SetMenuItemInfoW(menu, (UINT)i, TRUE, &set);
        saved.push_back(std::move(sv));
        items.push_back(std::move(item));
        if (mi.hSubMenu) Prepare(mi.hSubMenu, dc, items, saved);
    }
}

inline void Restore(std::vector<Saved>& saved) {
    for (Saved& sv : saved) {
        MENUITEMINFOW mi = {};
        mi.cbSize = sizeof(mi);
        mi.dwItemData = 0;
        if (sv.separator) {
            mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_DATA;
            mi.fType = MFT_SEPARATOR;
            mi.fState = MFS_ENABLED;
        } else {
            mi.fMask = MIIM_FTYPE | MIIM_STRING | MIIM_DATA;
            mi.fType = MFT_STRING | (sv.radio ? MFT_RADIOCHECK : 0);
            mi.dwTypeData = &sv.raw[0];
            mi.cch = (UINT)sv.raw.size();
        }
        SetMenuItemInfoW(sv.menu, sv.pos, TRUE, &mi);
    }
}

inline Item* ItemFrom(ULONG_PTR data) {
    Item* it = reinterpret_cast<Item*>(data);
    return it && it->magic == kMagic ? it : nullptr;
}

inline void DrawItem(const DRAWITEMSTRUCT& d, const Item& it) {
    HDC dc = d.hDC;
    const RECT rc = d.rcItem;
    const bool gray = (d.itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
    const bool hot = (d.itemState & ODS_SELECTED) != 0 && !it.separator;
    const bool checked = (d.itemState & ODS_CHECKED) != 0;
    Fill(dc, rc, hot && !gray ? kHover : kPanel);
    if (it.separator) {
        const int y = (rc.top + rc.bottom) / 2;
        Fill(dc, {rc.left + Px(8), y, rc.right - Px(8), y + 1}, kRule);
        return;
    }
    HGDIOBJ oldFont = SelectObject(dc, Context().font ? Context().font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    const int cy = (rc.top + rc.bottom) / 2;

    if (checked) {
        const COLORREF c = gray ? kDisabled : kAccent;
        const int cx = rc.left + Px(14);
        if (it.radio) {
            HBRUSH b = CreateSolidBrush(c);
            HGDIOBJ oldB = SelectObject(dc, b);
            HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
            const int r = Px(3);
            Ellipse(dc, cx - r, cy - r, cx + r + 1, cy + r + 1);
            SelectObject(dc, oldP);
            SelectObject(dc, oldB);
            DeleteObject(b);
        } else {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(2)), c);
            HGDIOBJ oldP = SelectObject(dc, pen);
            MoveToEx(dc, cx - Px(4), cy, nullptr);
            LineTo(dc, cx - Px(1), cy + Px(3));
            LineTo(dc, cx + Px(5), cy - Px(4));
            SelectObject(dc, oldP);
            DeleteObject(pen);
        }
    }

    SetTextColor(dc, gray ? kDisabled : (hot ? kTextBright : kText));
    RECT label = {rc.left + Px(30), rc.top, rc.right - Px(8), rc.bottom};
    DrawTextW(dc, it.text.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if (!it.accel.empty()) {
        SetTextColor(dc, gray ? kDisabled : kMuted);
        RECT accel = {rc.left, rc.top, rc.right - Px(it.popup ? 24 : 14), rc.bottom};
        DrawTextW(dc, it.accel.c_str(), -1, &accel, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);
    }
    if (it.popup) DrawDisclosure(dc, rc.right - Px(12), cy, Px(3), false, gray ? kDisabled : kMuted);
    SelectObject(dc, oldFont);
}

} // namespace menu_detail

// Handles WM_MEASUREITEM / WM_DRAWITEM for the items TrackMenu() prepared. Returns false for anything else.
inline bool HandleMenuMessage(UINT msg, LPARAM lparam, LRESULT* result) {
    if (msg == WM_MEASUREITEM) {
        auto* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (!m || m->CtlType != ODT_MENU) return false;
        const menu_detail::Item* it = menu_detail::ItemFrom(m->itemData);
        if (!it) return false;
        m->itemWidth = (UINT)it->width;
        m->itemHeight = (UINT)it->height;
        *result = TRUE;
        return true;
    }
    if (msg == WM_DRAWITEM) {
        auto* d = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (!d || d->CtlType != ODT_MENU) return false;
        const menu_detail::Item* it = menu_detail::ItemFrom(d->itemData);
        if (!it) return false;
        menu_detail::DrawItem(*d, *it);
        *result = TRUE;
        return true;
    }
    return false;
}

// TrackPopupMenu with the dark look. `flags` are the usual TPM_* flags (add TPM_RETURNCMD to get the command).
inline int TrackMenu(HMENU menu, UINT flags, int x, int y, HWND owner, int dpi) {
    menu_detail::Ctx& ctx = menu_detail::Context();
    ctx.dpi = dpi;
    ctx.font = MakeFont(dpi, 12, FW_NORMAL);
    std::vector<std::unique_ptr<menu_detail::Item>> items;
    std::vector<menu_detail::Saved> saved;
    HDC dc = GetDC(nullptr);
    HGDIOBJ oldFont = SelectObject(dc, ctx.font);
    menu_detail::Prepare(menu, dc, items, saved);
    SelectObject(dc, oldFont);
    ReleaseDC(nullptr, dc);

    const int cmd = TrackPopupMenu(menu, flags, x, y, 0, owner, nullptr);

    menu_detail::Restore(saved);
    DeleteObject(ctx.font);
    ctx.font = nullptr;
    return cmd;
}

} // namespace theme
} // namespace tbd
