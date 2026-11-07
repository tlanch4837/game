// TBD Engine project manager: lists projects, creates new ones, opens them in the engine.
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "../project.h"

#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <uxtheme.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace fs = std::filesystem;

namespace {

const wchar_t* kHubClass = L"TBDHubWindow";
const wchar_t* kHubTitle = L"TBD Engine - Projects";

enum {
    IDC_LIST = 100,
    IDC_NAME,
    IDC_NEW,
    IDC_ADD,
    IDC_REMOVE,
    IDC_OPEN,
};

struct Project {
    std::wstring name;
    std::wstring path;
    ULONGLONG lastOpened = 0; // FILETIME ticks, 0 = never
};

std::vector<Project> g_projects;
HWND g_hwnd, g_list, g_nameEdit, g_btnNew, g_btnAdd, g_btnRemove, g_btnOpen;
HFONT g_font;
int g_dpi = 96;

int S(int v) { return MulDiv(v, g_dpi, 96); }

// ---------------------------------------------------------------- persistence

std::wstring DataFilePath() {
    PWSTR appdata = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) dir = appdata;
    CoTaskMemFree(appdata);
    dir += L"\\TBDEngine";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\projects.txt";
}

std::wstring Clean(std::wstring s) {
    for (auto& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

void SaveProjects() {
    std::wstring text;
    for (const auto& p : g_projects)
        text += Clean(p.name) + L"\t" + Clean(p.path) + L"\t" + std::to_wstring(p.lastOpened) + L"\n";
    tbd::WriteAllText(DataFilePath(), tbd::WideToUtf8(text));
}

void LoadProjects() {
    g_projects.clear();
    std::string raw;
    if (!tbd::ReadAllText(DataFilePath(), raw)) return;
    std::wstring text = tbd::Utf8ToWide(raw);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t t1 = line.find(L'\t');
        size_t t2 = t1 == std::wstring::npos ? t1 : line.find(L'\t', t1 + 1);
        if (t2 == std::wstring::npos) continue;
        Project p;
        p.name = line.substr(0, t1);
        p.path = line.substr(t1 + 1, t2 - t1 - 1);
        p.lastOpened = _wcstoui64(line.c_str() + t2 + 1, nullptr, 10);
        if (!p.path.empty()) g_projects.push_back(std::move(p));
    }
}

// ----------------------------------------------------------------------- list

std::wstring FormatTime(ULONGLONG ticks) {
    if (ticks == 0) return L"Never";
    FILETIME utc, local;
    utc.dwLowDateTime = (DWORD)(ticks & 0xFFFFFFFF);
    utc.dwHighDateTime = (DWORD)(ticks >> 32);
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return L"?";
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

int Selected() { return ListView_GetNextItem(g_list, -1, LVNI_SELECTED); }

void UpdateButtons() {
    BOOL has = Selected() >= 0;
    EnableWindow(g_btnOpen, has);
    EnableWindow(g_btnRemove, has);
}

void SetCell(int row, int col, const std::wstring& text) {
    ListView_SetItemText(g_list, row, col, const_cast<LPWSTR>(text.c_str()));
}

// Rebuilds the list, most recently opened first, and reselects selectPath if given.
void Refresh(const std::wstring& selectPath = L"") {
    std::stable_sort(g_projects.begin(), g_projects.end(),
                     [](const Project& a, const Project& b) { return a.lastOpened > b.lastOpened; });
    ListView_DeleteAllItems(g_list);
    int select = -1;
    for (int i = 0; i < (int)g_projects.size(); ++i) {
        const Project& p = g_projects[i];
        LVITEMW item = {};
        item.mask = LVIF_TEXT;
        item.iItem = i;
        item.pszText = const_cast<LPWSTR>(p.name.c_str());
        ListView_InsertItem(g_list, &item);
        SetCell(i, 1, p.path);
        SetCell(i, 2, tbd::ProjectExists(p.path) ? FormatTime(p.lastOpened) : L"Folder missing");
        if (!selectPath.empty() && _wcsicmp(p.path.c_str(), selectPath.c_str()) == 0) select = i;
    }
    if (select >= 0) {
        ListView_SetItemState(g_list, select, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(g_list, select, FALSE);
    }
    UpdateButtons();
}

std::wstring Normalize(const std::wstring& path) {
    std::wstring p = fs::path(path).lexically_normal().wstring();
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}

void AddProject(const std::wstring& name, const std::wstring& path) {
    std::wstring norm = Normalize(path);
    for (const auto& p : g_projects)
        if (_wcsicmp(p.path.c_str(), norm.c_str()) == 0) {
            Refresh(norm);
            return;
        }
    Project p;
    p.name = name;
    p.path = norm;
    g_projects.push_back(std::move(p));
    SaveProjects();
    Refresh(norm);
}

// -------------------------------------------------------------------- actions

std::wstring PickFolder(HWND owner, const wchar_t* title) {
    std::wstring result;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return result;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(title);
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                result = p;
                CoTaskMemFree(p);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

void Error(const std::wstring& text) { MessageBoxW(g_hwnd, text.c_str(), kHubTitle, MB_OK | MB_ICONWARNING); }

void NewProject() {
    wchar_t buf[128] = {};
    GetWindowTextW(g_nameEdit, buf, 128);
    std::wstring name = buf;
    name.erase(0, name.find_first_not_of(L' '));
    name.erase(name.find_last_not_of(L' ') + 1);
    if (name.empty() || name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos || name.back() == L'.') {
        Error(L"Type a project name first. It can't contain \\ / : * ? \" < > | and can't end with a dot.");
        SetFocus(g_nameEdit);
        return;
    }
    std::wstring parent = PickFolder(g_hwnd, L"Choose the folder to create the project in");
    if (parent.empty()) return;

    fs::path folder = fs::path(parent) / name;
    std::error_code ec;
    if (fs::exists(folder, ec) && !fs::is_empty(folder, ec)) {
        Error(L"That folder already exists and isn't empty:\n" + folder.wstring());
        return;
    }
    fs::create_directories(folder / L"Assets", ec);
    if (!ec) fs::create_directories(folder / L"Scenes", ec);
    if (ec || !tbd::WriteProjectFile(folder.wstring(), name)) {
        Error(L"Couldn't create the project in:\n" + folder.wstring());
        return;
    }
    SetWindowTextW(g_nameEdit, L"");
    AddProject(name, folder.wstring());
}

void AddExisting() {
    std::wstring folder = PickFolder(g_hwnd, L"Choose a folder containing project.tbdproject");
    if (folder.empty()) return;
    std::wstring name;
    if (!tbd::ProjectExists(folder) || !tbd::ReadProjectName(folder, name)) {
        Error(L"No TBD Engine project found in that folder (project.tbdproject is missing or unreadable).");
        return;
    }
    AddProject(name, folder);
}

void RemoveSelected() {
    int sel = Selected();
    if (sel < 0) return;
    std::wstring msg = L"Remove \"" + g_projects[sel].name +
                       L"\" from the list?\n\nThe project's files on disk are not deleted.";
    if (MessageBoxW(g_hwnd, msg.c_str(), kHubTitle, MB_YESNO | MB_ICONQUESTION) != IDYES) return;
    g_projects.erase(g_projects.begin() + sel);
    SaveProjects();
    Refresh();
}

void OpenSelected() {
    int sel = Selected();
    if (sel < 0) return;
    Project proj = g_projects[sel];
    if (!tbd::ProjectExists(proj.path)) {
        std::wstring msg = L"The project folder is missing:\n" + proj.path + L"\n\nRemove it from the list?";
        if (MessageBoxW(g_hwnd, msg.c_str(), kHubTitle, MB_YESNO | MB_ICONWARNING) == IDYES) {
            g_projects.erase(g_projects.begin() + sel);
            SaveProjects();
            Refresh();
        }
        return;
    }

    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = fs::path(self).parent_path().wstring();
    std::wstring exe = dir + L"\\tbd_engine.exe";
    std::wstring cmd = L"\"" + exe + L"\" \"" + proj.path + L"\"";

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        Error(L"Couldn't start the engine:\n" + exe);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    g_projects[sel].lastOpened = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    SaveProjects();
    Refresh(proj.path);
}

// --------------------------------------------------------------------- window

void Layout() {
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int m = S(12), bar = S(56), bh = S(30), by = h - bar + (bar - bh) / 2;

    MoveWindow(g_list, m, m, w - 2 * m, h - bar - m, TRUE);
    int fixed = S(200) + S(150) + GetSystemMetrics(SM_CXVSCROLL) + S(6);
    ListView_SetColumnWidth(g_list, 0, S(200));
    ListView_SetColumnWidth(g_list, 1, std::max(S(120), w - 2 * m - fixed));
    ListView_SetColumnWidth(g_list, 2, S(150));

    int x = m;
    MoveWindow(g_nameEdit, x, by, S(220), bh, TRUE);
    x += S(220) + S(8);
    MoveWindow(g_btnNew, x, by, S(130), bh, TRUE);

    int r = w - m;
    r -= S(100);
    MoveWindow(g_btnOpen, r, by, S(100), bh, TRUE);
    r -= S(8) + S(100);
    MoveWindow(g_btnRemove, r, by, S(100), bh, TRUE);
    r -= S(8) + S(140);
    MoveWindow(g_btnAdd, r, by, S(140), bh, TRUE);
}

HWND MakeButton(const wchar_t* text, int id) {
    HWND b = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 0, 0,
                             g_hwnd, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
    return b;
}

void CreateControls() {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                             0, 0, 0, 0, g_hwnd, (HMENU)(INT_PTR)IDC_LIST, inst, nullptr);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    SetWindowTheme(g_list, L"Explorer", nullptr);

    const wchar_t* heads[] = {L"Name", L"Location", L"Last opened"};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.pszText = const_cast<LPWSTR>(heads[i]);
        col.cx = S(150);
        ListView_InsertColumn(g_list, i, &col);
    }

    g_nameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0, 0, 0, g_hwnd,
                                 (HMENU)(INT_PTR)IDC_NAME, inst, nullptr);
    SendMessageW(g_nameEdit, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_nameEdit, EM_LIMITTEXT, 100, 0);
    SendMessageW(g_nameEdit, EM_SETCUEBANNER, TRUE, (LPARAM)L"New project name");

    g_btnNew = MakeButton(L"New Project...", IDC_NEW);
    g_btnAdd = MakeButton(L"Add Existing...", IDC_ADD);
    g_btnRemove = MakeButton(L"Remove", IDC_REMOVE);
    g_btnOpen = MakeButton(L"Open", IDC_OPEN);
}

LRESULT CALLBACK HubProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_hwnd = hwnd;
        CreateControls();
        LoadProjects();
        Refresh();
        return 0;
    case WM_SETFOCUS:
        SetFocus(g_list);
        return 0;
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lparam);
        mmi->ptMinTrackSize.x = S(760);
        mmi->ptMinTrackSize.y = S(360);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case IDC_NEW: NewProject(); return 0;
        case IDC_ADD: AddExisting(); return 0;
        case IDC_REMOVE: RemoveSelected(); return 0;
        case IDC_OPEN: OpenSelected(); return 0;
        case IDOK: // Enter: create when typing a name, otherwise open
            if (GetFocus() == g_nameEdit) NewProject();
            else OpenSelected();
            return 0;
        }
        break;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lparam);
        if (hdr->idFrom != IDC_LIST) break;
        if (hdr->code == LVN_ITEMCHANGED) UpdateButtons();
        else if (hdr->code == NM_DBLCLK && reinterpret_cast<NMITEMACTIVATE*>(lparam)->iItem >= 0) OpenSelected();
        else if (hdr->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(lparam)->wVKey == VK_DELETE)
            RemoveSelected();
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDPIAware();
    g_dpi = (int)GetDpiForSystem();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HubProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = kHubClass;
    if (!RegisterClassExW(&wc)) return 1;

    RECT rect = {0, 0, S(900), S(520)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, kHubClass, kHubTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, instance,
                                nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return (int)msg.wParam;
}
