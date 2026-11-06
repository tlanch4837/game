// Project file format shared by the project manager and the engine.
// A project is a folder holding project.tbdproject (JSON) plus Assets/ and Scenes/.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

namespace tbd {

inline const wchar_t* kProjectFile = L"project.tbdproject";
inline const char* kEngineVersion = "0.1";

inline std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

inline bool ReadAllText(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (1 << 24)) {
        CloseHandle(h);
        return false;
    }
    out.assign((size_t)size.QuadPart, '\0');
    DWORD got = 0;
    BOOL ok = size.QuadPart == 0 ? TRUE : ReadFile(h, out.data(), (DWORD)size.QuadPart, &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return ok != FALSE;
}

inline bool WriteAllText(const std::wstring& path, const std::string& text) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    BOOL ok = text.empty() ? TRUE : WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr);
    CloseHandle(h);
    return ok && put == text.size();
}

inline std::wstring Join(const std::wstring& a, const std::wstring& b) {
    return (a.empty() || a.back() == L'\\') ? a + b : a + L'\\' + b;
}

inline std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out;
}

inline std::wstring ProjectFilePath(const std::wstring& folder) {
    std::wstring p = folder;
    if (!p.empty() && p.back() != L'\\' && p.back() != L'/') p += L'\\';
    return p + kProjectFile;
}

inline bool ProjectExists(const std::wstring& folder) {
    return GetFileAttributesW(ProjectFilePath(folder).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Reads the "name" field of folder\project.tbdproject.
inline bool ReadProjectName(const std::wstring& folder, std::wstring& name) {
    std::string text;
    if (!ReadAllText(ProjectFilePath(folder), text)) return false;
    size_t key = text.find("\"name\"");
    if (key == std::string::npos) return false;
    size_t colon = text.find(':', key);
    if (colon == std::string::npos) return false;
    size_t open = text.find('"', colon);
    if (open == std::string::npos) return false;
    std::string value;
    for (size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            value += text[++i];
        } else if (text[i] == '"') {
            name = Utf8ToWide(value);
            return !name.empty();
        } else {
            value += text[i];
        }
    }
    return false;
}

inline bool WriteProjectFile(const std::wstring& folder, const std::wstring& name) {
    std::string json = "{\n  \"name\": \"" + JsonEscape(WideToUtf8(name)) + "\",\n  \"engineVersion\": \"" +
                       kEngineVersion + "\"\n}\n";
    return WriteAllText(ProjectFilePath(folder), json);
}

} // namespace tbd
