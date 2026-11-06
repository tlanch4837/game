#define UNICODE
#define _UNICODE
#include "inspector.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "ui_theme.h"

namespace tbd {
namespace {

const wchar_t* kClass = L"TBDInspector";

const COLORREF kAxis[3] = {RGB(220, 76, 66), RGB(107, 199, 89), RGB(77, 128, 235)};

const wchar_t* kGroups[3] = {L"Location", L"Rotation", L"Scale"};
constexpr int kBaseId = 200;
// 96-dpi pixels, top to bottom: tab strip, entity name, section header, three field rows.
constexpr int kTabH = 26, kNameH = 34, kSectionH = 24, kRowH = 30, kBottomPad = 10;
constexpr int kLabelW = 84; // room for "Location" etc. left of the fields
constexpr int kFieldH = 22;
constexpr int kScriptRowH = 26, kMaxProps = 40, kPropBase = 300, kScriptsMaxH = 520;
const wchar_t* kUnknown = L"\x01"; // never equal to real field text, so the field gets rewritten

struct State {
    int dpi = 96;
    HFONT font = nullptr, boldFont = nullptr, nameFont = nullptr;
    HBRUSH editBrush = nullptr;
    HWND edit[9] = {};
    bool dirty[9] = {};
    bool updating = false; // set while we write text ourselves, so it isn't mistaken for typing
    std::wstring shown[9]; // text currently in each field, to skip redundant updates (kUnknown = rewrite it)
    std::string name;
    bool hasEntity = false;
    bool open = true; // the Transform section is expanded
    InspectorApply apply = nullptr;
    void* user = nullptr;
    // scripts section
    std::vector<InspectorScript> scripts;
    bool hasEntityForScripts = false;
    HWND propEdit[kMaxProps] = {};
    bool propDirty[kMaxProps] = {};
    std::wstring propShown[kMaxProps];
    int propScript[kMaxProps] = {}, propIndex[kMaxProps] = {}; // which script / prop each edit field currently edits
    int scroll = 0;
    InspectorScriptApply scriptApply = nullptr;
    void* scriptUser = nullptr;
};

int S(const State& s, int v) { return MulDiv(v, s.dpi, 96); }

int SectionTop(const State& s) { return S(s, kTabH + kNameH); }

// The framed box of a field (its axis stripe included), for a panel `w` pixels wide.
RECT FieldBox(const State& s, int w, int index) {
    const int group = index / 3, axis = index % 3;
    const int gap = S(s, 6), x0 = S(s, kLabelW), rightPad = S(s, 10);
    const int boxW = std::max(S(s, 24), (w - x0 - rightPad - 2 * gap) / 3);
    const int x = x0 + axis * (boxW + gap);
    const int rowTop = SectionTop(s) + S(s, kSectionH) + group * S(s, kRowH);
    const int y = rowTop + (S(s, kRowH) - S(s, kFieldH)) / 2;
    return {x, y, x + boxW, y + S(s, kFieldH)};
}

void PlaceFields(State& s, int w) {
    for (int i = 0; i < 9; ++i) {
        if (!s.edit[i]) continue;
        const RECT b = FieldBox(s, w, i);
        MoveWindow(s.edit[i], b.left + S(s, 8), b.top + S(s, 3), std::max(4, (int)(b.right - b.left) - S(s, 12)),
                   b.bottom - b.top - S(s, 6), TRUE);
        ShowWindow(s.edit[i], s.open ? SW_SHOWNA : SW_HIDE);
    }
}

// ---- scripts section geometry. Rows: per script one header row, then one row per declared prop.
int ScriptsTop(const State& s) { return SectionTop(s) + S(s, kSectionH) + (s.open ? 3 * S(s, kRowH) : 0); }
int RowsTop(const State& s) { return ScriptsTop(s) + S(s, kSectionH); }
int RowCount(const State& s) {
    int n = 0;
    for (const InspectorScript& sc : s.scripts) n += 1 + (int)sc.props.size();
    return n;
}
int ContentHeight(const State& s) { return RowCount(s) * S(s, kScriptRowH); }

std::wstring Wide(const std::string& t) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, t.c_str(), (int)t.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, t.c_str(), (int)t.size(), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string t(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), t.data(), n, nullptr, nullptr);
    return t;
}

RECT PropBox(const State& s, int w, int y) {
    const int x0 = S(s, kLabelW), pad = S(s, 10), fh = S(s, kFieldH);
    const int top = y + (S(s, kScriptRowH) - fh) / 2;
    return {x0, top, std::max(x0 + S(s, 30), w - pad), top + fh};
}

// Puts one edit field on each prop row, hiding those scrolled out of the visible part of the panel.
void PlaceProps(State& s, int w, int h) {
    const int top = RowsTop(s);
    int row = 0, e = 0;
    for (int si = 0; si < (int)s.scripts.size(); ++si) {
        ++row; // the script's header row
        for (int pi = 0; pi < (int)s.scripts[si].props.size(); ++pi, ++row) {
            if (e >= kMaxProps) break;
            const int y = top + row * S(s, kScriptRowH) - s.scroll;
            const bool visible = y >= top && y + S(s, kScriptRowH) <= h;
            const RECT b = PropBox(s, w, y);
            s.propScript[e] = si;
            s.propIndex[e] = pi;
            MoveWindow(s.propEdit[e], b.left + S(s, 8), b.top + S(s, 3), std::max(4, (int)(b.right - b.left) - S(s, 12)), b.bottom - b.top - S(s, 6), TRUE);
            ShowWindow(s.propEdit[e], visible ? SW_SHOWNA : SW_HIDE);
            ++e;
        }
    }
    for (; e < kMaxProps; ++e) {
        s.propScript[e] = -1;
        ShowWindow(s.propEdit[e], SW_HIDE);
    }
}

void ApplyProp(State& s, int e) {
    if (!s.propDirty[e]) return;
    s.propDirty[e] = false;
    s.propShown[e] = kUnknown;
    if (s.propScript[e] < 0 || !s.scriptApply) return;
    wchar_t buf[256];
    GetWindowTextW(s.propEdit[e], buf, 256);
    InspectorScriptEdit edit;
    edit.kind = InspectorScriptEdit::SetProp;
    edit.script = s.propScript[e];
    edit.prop = s.scripts[edit.script].props[s.propIndex[e]].name;
    edit.value = Narrow(buf);
    s.scriptApply(edit, s.scriptUser);
}

void Apply(State& s) {
    InspectorEdit edit;
    bool any = false;
    for (int i = 0; i < 9; ++i) {
        if (!s.dirty[i]) continue;
        s.dirty[i] = false;
        s.shown[i] = kUnknown; // force the field to show the real value again
        wchar_t buf[64];
        GetWindowTextW(s.edit[i], buf, 64);
        wchar_t* end = nullptr;
        const float v = wcstof(buf, &end);
        if (end != buf && std::isfinite(v)) {
            edit.set[i] = true;
            edit.value[i] = v;
            any = true;
        }
    }
    if (any && s.apply) s.apply(edit, s.user);
}

void Revert(State& s) {
    for (int i = 0; i < 9; ++i) {
        s.dirty[i] = false;
        s.shown[i] = kUnknown;
    }
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    State& s = *reinterpret_cast<State*>(data);
    switch (msg) {
    case WM_KEYDOWN:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
            if (wparam == VK_RETURN) Apply(s);
            else Revert(s);
            SetFocus(GetAncestor(hwnd, GA_ROOT)); // back to the viewport so shortcuts work again
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0; // no beep
        break;
    case WM_KILLFOCUS:
        Apply(s);
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK PropEditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam, UINT_PTR id, DWORD_PTR data) {
    State& s = *reinterpret_cast<State*>(data);
    const int e = (int)id;
    switch (msg) {
    case WM_KEYDOWN:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) {
            if (wparam == VK_RETURN) ApplyProp(s, e);
            else { s.propDirty[e] = false; s.propShown[e] = kUnknown; }
            SetFocus(GetAncestor(hwnd, GA_ROOT));
            return 0;
        }
        break;
    case WM_CHAR:
        if (wparam == VK_RETURN || wparam == VK_ESCAPE) return 0;
        break;
    case WM_KILLFOCUS:
        ApplyProp(s, e);
        break;
    }
    return DefSubclassProc(hwnd, msg, wparam, lparam);
}

void Paint(HWND hwnd, State& s) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = rc.right, h = rc.bottom;

    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    HGDIOBJ oldFont = SelectObject(mem, s.font);
    theme::Fill(mem, rc, theme::kPanel);
    SetBkMode(mem, TRANSPARENT);

    // Tab strip with the panel's title.
    theme::Fill(mem, {0, 0, w, S(s, kTabH)}, theme::kWindow);
    SelectObject(mem, s.boldFont);
    theme::Fill(mem, {S(s, 6), S(s, 4), S(s, 6) + S(s, 78) + S(s, 20), S(s, kTabH)}, theme::kPanel);
    SetTextColor(mem, theme::kTextBright);
    theme::DrawCaps(mem, {S(s, 16), S(s, 4), S(s, 16) + S(s, 88), S(s, kTabH)}, L"DETAILS");

    // The selected entity's name.
    SelectObject(mem, s.nameFont);
    SetTextColor(mem, s.hasEntity ? theme::kTextBright : theme::kMuted);
    RECT name = {S(s, 14), S(s, kTabH), w - S(s, 10), S(s, kTabH + kNameH)};
    const std::wstring title = s.hasEntity ? std::wstring(s.name.begin(), s.name.end()) : L"Nothing selected";
    DrawTextW(mem, title.c_str(), -1, &name, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);

    // Collapsible section header.
    const int secTop = SectionTop(s);
    theme::Fill(mem, {0, secTop, w, secTop + S(s, kSectionH)}, theme::kRaised);
    theme::Fill(mem, {0, secTop, w, secTop + 1}, theme::kLine);
    theme::DrawDisclosure(mem, S(s, 14), secTop + S(s, kSectionH) / 2, S(s, 4), s.open, theme::kMuted);
    SelectObject(mem, s.boldFont);
    SetTextColor(mem, theme::kText);
    theme::DrawCaps(mem, {S(s, 26), secTop, w - S(s, 8), secTop + S(s, kSectionH)}, L"TRANSFORM");

    if (s.open) {
        SelectObject(mem, s.font);
        for (int g = 0; g < 3; ++g) {
            const int rowTop = secTop + S(s, kSectionH) + g * S(s, kRowH);
            SetTextColor(mem, s.hasEntity ? theme::kText : theme::kDisabled);
            RECT label = {S(s, 14), rowTop, S(s, kLabelW), rowTop + S(s, kRowH)};
            DrawTextW(mem, kGroups[g], -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
            for (int a = 0; a < 3; ++a) {
                const int i = g * 3 + a;
                const RECT box = FieldBox(s, w, i);
                theme::Fill(mem, box, theme::kWindow);
                theme::Frame(mem, box, s.hasEntity && GetFocus() == s.edit[i] ? theme::kAccent : theme::kRule);
                theme::Fill(mem, {box.left + 1, box.top + 1, box.left + 1 + S(s, 3), box.bottom - 1},
                            s.hasEntity ? kAxis[a] : theme::kDisabled);
            }
        }
    }
    // Scripts section: header with a + button, then a header row per script (with an x) and a field row per declared prop.
    {
        const int top = ScriptsTop(s);
        theme::Fill(mem, {0, top, w, top + S(s, kSectionH)}, theme::kRaised);
        theme::Fill(mem, {0, top, w, top + 1}, theme::kLine);
        SelectObject(mem, s.boldFont);
        SetTextColor(mem, theme::kText);
        theme::DrawCaps(mem, {S(s, 14), top, w - S(s, 60), top + S(s, kSectionH)}, L"SCRIPTS");
        SetTextColor(mem, s.hasEntityForScripts ? theme::kAccent : theme::kDisabled);
        theme::DrawCaps(mem, {w - S(s, 64), top, w - S(s, 8), top + S(s, kSectionH)}, L"+ ADD", DT_SINGLELINE | DT_VCENTER | DT_RIGHT);

        const int rowsTop = RowsTop(s);
        const int saved = SaveDC(mem);
        IntersectClipRect(mem, 0, rowsTop, w, h);
        SelectObject(mem, s.font);
        int row = 0;
        for (const InspectorScript& sc : s.scripts) {
            int y = rowsTop + row * S(s, kScriptRowH) - s.scroll;
            ++row;
            SetTextColor(mem, sc.missing ? RGB(220, 76, 66) : theme::kTextBright);
            const std::wstring title = Wide(sc.path) + (sc.missing ? L"  (missing)" : L"");
            RECT label = {S(s, 14), y, w - S(s, 34), y + S(s, kScriptRowH)};
            DrawTextW(mem, title.c_str(), -1, &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
            SetTextColor(mem, theme::kMuted);
            RECT x = {w - S(s, 30), y, w - S(s, 8), y + S(s, kScriptRowH)};
            DrawTextW(mem, L"\x00d7", -1, &x, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
            for (const InspectorScriptProp& pr : sc.props) {
                y = rowsTop + row * S(s, kScriptRowH) - s.scroll;
                ++row;
                SetTextColor(mem, pr.overridden ? theme::kTextBright : theme::kText);
                RECT pl = {S(s, 24), y, S(s, kLabelW), y + S(s, kScriptRowH)};
                DrawTextW(mem, Wide(pr.name).c_str(), -1, &pl, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
                const RECT box = PropBox(s, w, y);
                theme::Fill(mem, box, theme::kWindow);
                theme::Frame(mem, box, theme::kRule);
            }
        }
        RestoreDC(mem, saved);
    }
    theme::Fill(mem, {0, 0, 1, h}, theme::kLine); // edge towards the viewport

    BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK InspectorProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT:
        if (s) Paint(hwnd, *s);
        else ValidateRect(hwnd, nullptr);
        return 0;
    case WM_SIZE:
        if (s) {
            PlaceFields(*s, LOWORD(lparam));
            PlaceProps(*s, LOWORD(lparam), HIWORD(lparam));
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        if (s) {
            const int y = GET_Y_LPARAM(lparam);
            if (y >= SectionTop(*s) && y < SectionTop(*s) + S(*s, kSectionH)) { // click on the section header
                s->open = !s->open;
                RECT rc;
                GetClientRect(hwnd, &rc);
                PlaceFields(*s, rc.right);
                PlaceProps(*s, rc.right, rc.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (s->scriptApply && s->hasEntityForScripts) {
                const int x = GET_X_LPARAM(lparam);
                RECT rc;
                GetClientRect(hwnd, &rc);
                const int top = ScriptsTop(*s);
                if (y >= top && y < top + S(*s, kSectionH) && x > rc.right - S(*s, 70)) { // + ADD
                    InspectorScriptEdit edit;
                    edit.kind = InspectorScriptEdit::Add;
                    s->scriptApply(edit, s->scriptUser);
                } else if (y >= RowsTop(*s) && x > rc.right - S(*s, 34)) { // x on a script's header row
                    int row = 0;
                    for (int si = 0; si < (int)s->scripts.size(); ++si) {
                        const int ry = RowsTop(*s) + row * S(*s, kScriptRowH) - s->scroll;
                        if (y >= ry && y < ry + S(*s, kScriptRowH)) {
                            InspectorScriptEdit edit;
                            edit.kind = InspectorScriptEdit::Remove;
                            edit.script = si;
                            s->scriptApply(edit, s->scriptUser);
                            break;
                        }
                        row += 1 + (int)s->scripts[si].props.size();
                    }
                }
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (s) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            const int maxScroll = std::max(0, ContentHeight(*s) - ((int)rc.bottom - RowsTop(*s)));
            s->scroll = std::clamp(s->scroll - GET_WHEEL_DELTA_WPARAM(wparam) * S(*s, kScriptRowH) / WHEEL_DELTA, 0, maxScroll);
            PlaceProps(*s, rc.right, rc.bottom);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_COMMAND:
        if (s) {
            const int pe = (int)LOWORD(wparam) - kPropBase;
            if (pe >= 0 && pe < kMaxProps) {
                if (HIWORD(wparam) == EN_CHANGE && !s->updating) s->propDirty[pe] = true;
                return 0;
            }
            const int index = (int)LOWORD(wparam) - kBaseId;
            if (index >= 0 && index < 9) {
                const int code = HIWORD(wparam);
                if (code == EN_CHANGE && !s->updating) s->dirty[index] = true;
                else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateRect(hwnd, nullptr, FALSE); // focus frame
            }
        }
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORSTATIC:
        if (s) {
            SetTextColor((HDC)wparam, theme::kText);
            SetBkColor((HDC)wparam, theme::kWindow);
            return (LRESULT)s->editBrush;
        }
        break;
    case WM_NCDESTROY:
        if (s) {
            DeleteObject(s->font);
            DeleteObject(s->boldFont);
            DeleteObject(s->nameFont);
            DeleteObject(s->editBrush);
            delete s;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int Inspector_Height(int dpi) {
    return MulDiv(kTabH + kNameH + kSectionH + 3 * kRowH + kBottomPad, dpi, 96);
}

HWND Inspector_Create(HWND parent, HINSTANCE instance, int dpi, InspectorApply apply, void* user) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = InspectorProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        if (!RegisterClassExW(&wc)) return nullptr;
        registered = true;
    }

    auto* s = new State;
    for (auto& text : s->shown) text = kUnknown;
    s->dpi = dpi;
    s->apply = apply;
    s->user = user;
    s->font = theme::MakeFont(dpi, 12, FW_NORMAL);
    s->boldFont = theme::MakeFont(dpi, 11, FW_SEMIBOLD);
    s->nameFont = theme::MakeFont(dpi, 13, FW_SEMIBOLD);
    s->editBrush = CreateSolidBrush(theme::kWindow);

    HWND hwnd = CreateWindowExW(0, kClass, L"Details", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, MulDiv(300, dpi, 96),
                                Inspector_Height(dpi), parent, nullptr, instance, s);
    if (!hwnd) {
        DeleteObject(s->font);
        DeleteObject(s->boldFont);
        DeleteObject(s->nameFont);
        DeleteObject(s->editBrush);
        delete s;
        return nullptr;
    }
    for (int i = 0; i < 9; ++i) {
        s->edit[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 40, 16,
                                     hwnd, (HMENU)(INT_PTR)(kBaseId + i), instance, nullptr);
        SendMessageW(s->edit[i], WM_SETFONT, (WPARAM)s->font, TRUE);
        SendMessageW(s->edit[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SendMessageW(s->edit[i], EM_LIMITTEXT, 16, 0);
        SetWindowSubclass(s->edit[i], EditProc, (UINT_PTR)i, (DWORD_PTR)s);
    }
    for (int i = 0; i < kMaxProps; ++i) {
        s->propShown[i] = kUnknown;
        s->propScript[i] = -1;
        s->propEdit[i] = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 40, 16, hwnd, (HMENU)(INT_PTR)(kPropBase + i), instance, nullptr);
        SendMessageW(s->propEdit[i], WM_SETFONT, (WPARAM)s->font, TRUE);
        SendMessageW(s->propEdit[i], EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(0, 0));
        SendMessageW(s->propEdit[i], EM_LIMITTEXT, 120, 0);
        SetWindowSubclass(s->propEdit[i], PropEditProc, (UINT_PTR)i, (DWORD_PTR)s);
    }
    RECT rc;
    GetClientRect(hwnd, &rc);
    PlaceFields(*s, rc.right);
    PlaceProps(*s, rc.right, rc.bottom);
    return hwnd;
}

void Inspector_Show(HWND inspector, const char* name, const float values[9]) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(inspector, GWLP_USERDATA));
    if (!s) return;

    const bool has = name != nullptr;
    const std::string newName = has ? name : "";
    if (has != s->hasEntity || newName != s->name) {
        s->hasEntity = has;
        s->name = newName;
        InvalidateRect(inspector, nullptr, FALSE);
        for (int i = 0; i < 9; ++i) {
            EnableWindow(s->edit[i], has);
            s->shown[i] = kUnknown;
        }
    }
    s->updating = true;
    HWND focus = GetFocus();
    for (int i = 0; i < 9; ++i) {
        if (focus == s->edit[i]) continue; // don't fight someone who is typing
        wchar_t text[32] = L"";
        if (has) swprintf_s(text, i >= 3 && i < 6 ? L"%.1f" : L"%.2f", values[i]);
        if (s->shown[i] != text) {
            s->shown[i] = text;
            SetWindowTextW(s->edit[i], text);
        }
    }
    s->updating = false;
}

void Inspector_SetScriptHandler(HWND inspector, InspectorScriptApply apply, void* user) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(inspector, GWLP_USERDATA));
    if (!s) return;
    s->scriptApply = apply;
    s->scriptUser = user;
}

void Inspector_ShowScripts(HWND inspector, bool hasEntity, const std::vector<InspectorScript>& scripts) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(inspector, GWLP_USERDATA));
    if (!s) return;
    const std::vector<InspectorScript> none;
    const std::vector<InspectorScript>& now = hasEntity ? scripts : none;
    bool sameShape = now.size() == s->scripts.size() && hasEntity == s->hasEntityForScripts;
    for (size_t i = 0; sameShape && i < now.size(); ++i) {
        sameShape = now[i].path == s->scripts[i].path && now[i].missing == s->scripts[i].missing && now[i].props.size() == s->scripts[i].props.size();
        for (size_t k = 0; sameShape && k < now[i].props.size(); ++k)
            sameShape = now[i].props[k].name == s->scripts[i].props[k].name && now[i].props[k].overridden == s->scripts[i].props[k].overridden;
    }
    s->scripts = now;
    s->hasEntityForScripts = hasEntity;
    RECT rc;
    GetClientRect(inspector, &rc);
    if (!sameShape) {
        s->scroll = std::clamp(s->scroll, 0, std::max(0, ContentHeight(*s) - ((int)rc.bottom - RowsTop(*s))));
        for (int e = 0; e < kMaxProps; ++e) s->propShown[e] = kUnknown;
        PlaceProps(*s, rc.right, rc.bottom);
        InvalidateRect(inspector, nullptr, FALSE);
    }
    s->updating = true;
    HWND focus = GetFocus();
    for (int e = 0; e < kMaxProps; ++e) {
        if (s->propScript[e] < 0 || focus == s->propEdit[e]) continue; // not in use, or someone is typing
        const std::wstring text = Wide(s->scripts[s->propScript[e]].props[s->propIndex[e]].value);
        if (s->propShown[e] != text) {
            s->propShown[e] = text;
            SetWindowTextW(s->propEdit[e], text.c_str());
        }
    }
    s->updating = false;
}

int Inspector_DesiredHeight(HWND inspector) {
    State* s = reinterpret_cast<State*>(GetWindowLongPtrW(inspector, GWLP_USERDATA));
    if (!s) return 0;
    const int content = RowsTop(*s) + ContentHeight(*s) + S(*s, kBottomPad);
    return std::min(content, S(*s, kScriptsMaxH));
}

} // namespace tbd
