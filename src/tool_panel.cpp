#define UNICODE
#define _UNICODE
#include "tool_panel.h"

#include "mocap/face_mocap_ui.h"
#include "mocap/face_preview.h"
#include "ui_theme.h"

#include <windowsx.h>

#include <algorithm>
#include <string>
#include <vector>

namespace tbd {
namespace {

const wchar_t* kBarClass = L"TBDToolbar";
constexpr UINT_PTR kPollTimer = 1;
constexpr int kMenuRow = 24; // 96-dpi pixels: the menu strip
constexpr int kToolRow = 38; // and the toolbar under it

// One clickable thing on the bar.
enum class Kind { Menu, Tool, Dropdown, Command, Transport, Separator };

struct Item {
    Kind kind = Kind::Separator;
    RECT rc = {};
    std::wstring label;
    int a = 0; // Menu: popup index. Tool / Dropdown: tool set. Command: 0 save, 1 frame, 2 browser, 3 pop out. Transport: 0 play, 1 pause, 2 stop
    int b = 0; // Tool: index inside its set
};

struct State {
    ToolbarHost host;
    int dpi = 96;
    HFONT font = nullptr;
    std::vector<Item> items;
    int hover = -1;    // index into items
    int openItem = -1; // item whose popup is showing
    bool tracking = false;
    unsigned signature = 0;
};

int S(const State& s, int v) { return MulDiv(v, s.dpi, 96); }

int TextW(HWND hwnd, HFONT font, const std::wstring& text) {
    HDC dc = GetDC(hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text.c_str(), (int)text.size(), &sz);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
    return sz.cx;
}

int TransportState(const State& s) { return s.host.transportState ? s.host.transportState(s.host.user) : 0; }
bool BrowserShown(const State& s) { return s.host.browserShown && s.host.browserShown(s.host.user); }
bool Floating(const State& s) { return s.host.viewportFloating && s.host.viewportFloating(s.host.user); }

bool ActiveInSet(const State& s, int set) {
    if (!s.host.tools) return false;
    const auto& cats = s.host.tools->Categories();
    if (set < 0 || set >= (int)cats.size()) return false;
    for (const Tool& t : cats[set].tools)
        if (t.id == s.host.tools->Active()) return true;
    return false;
}

// Everything the bar shows that can change without a click on it; a change repaints.
unsigned Signature(const State& s) {
    unsigned v = s.host.tools ? (unsigned)s.host.tools->Active() + 1u : 0u;
    v = v * 4u + (unsigned)TransportState(s);
    v = v * 2u + (BrowserShown(s) ? 1u : 0u);
    v = v * 2u + (Floating(s) ? 1u : 0u);
    return v;
}

// Lays the items out for the current width: menu strip on top; below it the tools on the left, the
// transport controls centred and the shortcuts on the right.
void BuildItems(HWND hwnd, State& s) {
    s.items.clear();
    s.hover = -1;
    RECT client;
    GetClientRect(hwnd, &client);
    const int w = client.right;
    auto add = [&](Kind kind, RECT rc, const std::wstring& label, int a, int b) {
        Item it;
        it.kind = kind;
        it.rc = rc;
        it.label = label;
        it.a = a;
        it.b = b;
        s.items.push_back(it);
    };

    int x = S(s, 6);
    if (s.host.menu) {
        const int n = GetMenuItemCount(s.host.menu);
        for (int i = 0; i < n; ++i) {
            wchar_t text[64] = {};
            GetMenuStringW(s.host.menu, (UINT)i, text, 64, MF_BYPOSITION);
            const int bw = TextW(hwnd, s.font, text) + S(s, 16);
            add(Kind::Menu, {x, 0, x + bw, S(s, kMenuRow)}, text, i, 0);
            x += bw;
        }
    }

    const int btnH = S(s, 26), btnTop = S(s, kMenuRow) + (S(s, kToolRow) - btnH) / 2;
    x = S(s, 8);
    auto addButton = [&](Kind kind, const std::wstring& label, int a, int b, bool arrow) {
        const int bw = TextW(hwnd, s.font, label) + S(s, 20) + (arrow ? S(s, 12) : 0);
        add(kind, {x, btnTop, x + bw, btnTop + btnH}, label, a, b);
        x += bw + S(s, 2);
    };
    auto addSeparator = [&]() {
        x += S(s, 4);
        add(Kind::Separator, {x, btnTop + S(s, 3), x + 1, btnTop + btnH - S(s, 3)}, L"", 0, 0);
        x += S(s, 7);
    };

    addButton(Kind::Command, L"Save", 0, 0, false);
    if (s.host.tools) {
        const auto& cats = s.host.tools->Categories();
        for (int c = 0; c < (int)cats.size(); ++c) {
            if (c <= 1) addSeparator();
            if (c == 0) {
                for (int t = 0; t < (int)cats[c].tools.size(); ++t) addButton(Kind::Tool, cats[c].tools[t].name, c, t, false);
            } else {
                addButton(Kind::Dropdown, cats[c].name, c, 0, true);
            }
        }
    }
    const int leftEnd = x;

    const int tw = S(s, 34), gap = S(s, 2);
    const int groupW = 3 * tw + 2 * gap;
    const int cx0 = std::max((w - groupW) / 2, leftEnd + S(s, 12));
    for (int i = 0; i < 3; ++i) {
        const int left = cx0 + i * (tw + gap);
        add(Kind::Transport, {left, btnTop, left + tw, btnTop + btnH}, L"", i, 0);
    }

    const wchar_t* names[3] = {L"Frame", L"Content", L"Pop Out"};
    int widths[3] = {};
    int total = 0;
    for (int i = 0; i < 3; ++i) {
        widths[i] = TextW(hwnd, s.font, names[i]) + S(s, 20);
        total += widths[i] + gap;
    }
    int rx = std::max(cx0 + groupW + S(s, 12), w - S(s, 10) - total);
    for (int i = 0; i < 3; ++i) {
        add(Kind::Command, {rx, btnTop, rx + widths[i], btnTop + btnH}, names[i], i + 1, 0);
        rx += widths[i] + gap;
    }
}

int HitTest(const State& s, int x, int y) {
    const POINT pt = {x, y};
    for (int i = 0; i < (int)s.items.size(); ++i)
        if (s.items[i].kind != Kind::Separator && PtInRect(&s.items[i].rc, pt)) return i;
    return -1;
}

void DrawTransportIcon(HDC dc, const State& s, const RECT& rc, int which, int state) {
    const int cx = (rc.left + rc.right) / 2, cy = (rc.top + rc.bottom) / 2, r = S(s, 6);
    const bool lit = (which == 0 && state == 1) || (which == 1 && state == 2);
    const COLORREF color = lit ? theme::kAccent : (which == 2 && state == 0 ? theme::kMuted : theme::kText);
    if (which == 0) {
        const POINT tri[3] = {{cx - r / 2, cy - r}, {cx - r / 2, cy + r}, {cx + r, cy}};
        theme::FillPolygon(dc, tri, 3, color);
    } else if (which == 1) {
        theme::Fill(dc, {cx - r + 1, cy - r, cx - r / 3, cy + r + 1}, color);
        theme::Fill(dc, {cx + r / 3, cy - r, cx + r, cy + r + 1}, color);
    } else {
        theme::Fill(dc, {cx - r + 1, cy - r + 1, cx + r, cy + r}, color);
    }
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

    const int menuH = S(s, kMenuRow);
    theme::Fill(mem, {0, 0, w, menuH}, theme::kWindow);
    theme::Fill(mem, {0, menuH, w, h}, theme::kPanel);
    theme::Fill(mem, {0, h - 1, w, h}, theme::kLine);

    // Backdrop behind the transport controls.
    RECT group = {};
    for (const Item& it : s.items)
        if (it.kind == Kind::Transport) {
            if (group.right == 0) group = it.rc;
            else UnionRect(&group, &group, &it.rc);
        }
    if (group.right > 0) theme::Fill(mem, {group.left - S(s, 3), group.top - S(s, 2), group.right + S(s, 3), group.bottom + S(s, 2)}, theme::kWindow);

    const ToolId active = s.host.tools ? s.host.tools->Active() : ToolId::Select;
    const int state = TransportState(s);
    const DWORD centered = DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX;
    for (int i = 0; i < (int)s.items.size(); ++i) {
        const Item& it = s.items[i];
        const bool hot = i == s.hover, open = i == s.openItem;
        RECT rc = it.rc;
        switch (it.kind) {
        case Kind::Separator: theme::Fill(mem, rc, theme::kRule); break;
        case Kind::Menu:
            if (open) theme::Fill(mem, rc, theme::kRaised);
            else if (hot) theme::Fill(mem, rc, theme::kHover);
            SetTextColor(mem, hot || open ? theme::kTextBright : theme::kText);
            DrawTextW(mem, it.label.c_str(), -1, &rc, centered);
            break;
        case Kind::Tool: {
            const Tool& tool = s.host.tools->Categories()[it.a].tools[it.b];
            const bool on = tool.id == active;
            if (on) theme::Fill(mem, rc, theme::kAccent);
            else if (hot) theme::Fill(mem, rc, theme::kHover);
            SetTextColor(mem, on ? RGB(255, 255, 255) : (hot ? theme::kTextBright : theme::kText));
            DrawTextW(mem, it.label.c_str(), -1, &rc, centered);
            break;
        }
        case Kind::Dropdown: {
            if (open) theme::Fill(mem, rc, theme::kRaised);
            else if (hot) theme::Fill(mem, rc, theme::kHover);
            if (ActiveInSet(s, it.a)) theme::Fill(mem, {rc.left, rc.bottom - S(s, 2), rc.right, rc.bottom}, theme::kAccent);
            SetTextColor(mem, hot || open ? theme::kTextBright : theme::kText);
            RECT text = {rc.left, rc.top, rc.right - S(s, 12), rc.bottom};
            DrawTextW(mem, it.label.c_str(), -1, &text, centered);
            theme::DrawDisclosure(mem, rc.right - S(s, 12), (rc.top + rc.bottom) / 2, S(s, 3), true, theme::kMuted);
            break;
        }
        case Kind::Command: {
            const bool on = (it.a == 2 && BrowserShown(s)) || (it.a == 3 && Floating(s));
            if (on) theme::Fill(mem, rc, theme::kRaised);
            if (hot) theme::Fill(mem, rc, theme::kHover);
            if (on) theme::Fill(mem, {rc.left, rc.bottom - S(s, 2), rc.right, rc.bottom}, theme::kAccent);
            SetTextColor(mem, hot ? theme::kTextBright : theme::kText);
            DrawTextW(mem, it.label.c_str(), -1, &rc, centered);
            break;
        }
        case Kind::Transport:
            if (hot) theme::Fill(mem, rc, theme::kHover);
            DrawTransportIcon(mem, s, rc, it.a, state);
            break;
        }
    }

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

// Runs a tool: action tools open their dialog or workflow, the rest become the active tool.
void InvokeTool(HWND hwnd, ToolSet& tools, int set, int index) {
    auto& cats = tools.Categories();
    if (set < 0 || set >= (int)cats.size() || index < 0 || index >= (int)cats[set].tools.size()) return;
    const ToolId id = cats[set].tools[index].id;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    HINSTANCE instance = (HINSTANCE)GetWindowLongPtrW(hwnd, GWLP_HINSTANCE);
    if (id == ToolId::FaceMocap) FaceMocap_Open(root, instance);
    else if (id == ToolId::FaceMocapPreview) FaceMocapPreview_Pick(root, instance);
    else if (id == ToolId::FaceDrive || id == ToolId::FaceDriveStop || id == ToolId::FaceLive) {
        if (tools.onAction) tools.onAction(id);
    } else tools.SetActive(id);
}

// Shows `popup` under item `index` and returns the chosen command (0 = none). The item stays lit meanwhile.
int TrackUnder(HWND hwnd, State& s, int index, HMENU popup) {
    const RECT rc = s.items[index].rc;
    POINT pt = {rc.left, rc.bottom};
    ClientToScreen(hwnd, &pt);
    s.openItem = index;
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
    const int cmd = theme::TrackMenu(popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y,
                                     hwnd, s.dpi);
    s.openItem = -1;
    s.hover = -1; // the pointer moved while the menu was up
    InvalidateRect(hwnd, nullptr, FALSE);
    return cmd;
}

void Activate(HWND hwnd, State& s, int index) {
    const Item it = s.items[index]; // a copy: a menu runs a message loop, which may relayout the bar
    switch (it.kind) {
    case Kind::Menu: {
        HMENU popup = GetSubMenu(s.host.menu, it.a);
        if (!popup) return;
        if (s.host.beforeMenu) s.host.beforeMenu(s.host.user);
        const int cmd = TrackUnder(hwnd, s, index, popup);
        if (cmd > 0) SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(cmd, 0), 0);
        break;
    }
    case Kind::Tool:
        if (s.host.tools) InvokeTool(hwnd, *s.host.tools, it.a, it.b);
        break;
    case Kind::Dropdown: {
        if (!s.host.tools) return;
        const auto& cats = s.host.tools->Categories();
        if (it.a < 0 || it.a >= (int)cats.size()) return;
        HMENU menu = CreatePopupMenu();
        for (int t = 0; t < (int)cats[it.a].tools.size(); ++t) {
            const Tool& tool = cats[it.a].tools[t];
            std::wstring label = tool.name;
            if (tool.key) label += std::wstring(L"\t") + tool.key;
            AppendMenuW(menu, MF_STRING | (tool.id == s.host.tools->Active() ? MF_CHECKED : 0), (UINT_PTR)(t + 1), label.c_str());
        }
        const int cmd = TrackUnder(hwnd, s, index, menu);
        DestroyMenu(menu);
        if (cmd > 0 && s.host.tools) InvokeTool(hwnd, *s.host.tools, it.a, cmd - 1);
        break;
    }
    case Kind::Command: {
        const UINT ids[4] = {s.host.cmdSave, s.host.cmdFrame, s.host.cmdBrowser, s.host.cmdPopout};
        if (it.a >= 0 && it.a < 4 && ids[it.a]) SendMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(ids[it.a], 0), 0);
        break;
    }
    case Kind::Transport:
        if (s.host.transport) s.host.transport(it.a, s.host.user);
        break;
    case Kind::Separator: break;
    }
    InvalidateRect(hwnd, nullptr, FALSE);
}

LRESULT CALLBACK BarProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
        if (s && s->font) {
            BuildItems(hwnd, *s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (s && wparam == kPollTimer) {
            const unsigned sig = Signature(*s);
            if (sig != s->signature) {
                s->signature = sig;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }
        return 0;
    case WM_MOUSEMOVE: {
        if (!s) break;
        const int hit = HitTest(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        if (hit != s->hover) {
            s->hover = hit;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (!s->tracking) {
            TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
            s->tracking = TrackMouseEvent(&t) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s) {
            s->tracking = false;
            s->hover = -1;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (s) {
            const int hit = HitTest(*s, GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (hit >= 0) Activate(hwnd, *s, hit);
        }
        return 0;
    case WM_NCDESTROY:
        KillTimer(hwnd, kPollTimer);
        if (s) {
            DeleteObject(s->font);
            delete s;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int Toolbar_Height(int dpi) { return MulDiv(kMenuRow + kToolRow, dpi, 96); }

void Toolbar_Refresh(HWND toolbar) {
    if (toolbar) InvalidateRect(toolbar, nullptr, FALSE);
}

HWND Toolbar_Create(HWND parent, HINSTANCE instance, int dpi, const ToolbarHost& host) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = BarProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kBarClass;
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }

    auto* s = new State;
    s->host = host;
    s->dpi = dpi;
    s->font = theme::MakeFont(dpi, 12, FW_NORMAL);

    HWND hwnd = CreateWindowExW(0, kBarClass, L"Toolbar", WS_CHILD | WS_VISIBLE, 0, 0, 100, Toolbar_Height(dpi), parent,
                                nullptr, instance, s);
    if (!hwnd) { // the window never took ownership of the state
        DeleteObject(s->font);
        delete s;
        return nullptr;
    }
    BuildItems(hwnd, *s);
    s->signature = Signature(*s);
    SetTimer(hwnd, kPollTimer, 100, nullptr);
    return hwnd;
}

} // namespace tbd
