// Project file format shared by the project manager and the engine.
// A project is a folder holding project.tbdproject (JSON) plus Assets/ and Scenes/.
#pragma once
#include "platform/platform.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace tbd {

inline const wchar_t* kProjectFile = L"project.tbdproject";
inline const char* kEngineVersion = "0.1";

inline std::wstring Utf8ToWide(const std::string& s) { return platform::Utf8ToWide(s); }

inline std::string WideToUtf8(const std::wstring& w) { return platform::WideToUtf8(w); }

inline bool ReadAllText(const std::wstring& path, std::string& out) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff size = f.tellg();
    if (size < 0 || size > (1 << 24)) return false;
    f.seekg(0, std::ios::beg);
    out.assign((size_t)size, '\0');
    if (size > 0) f.read(out.data(), size);
    out.resize((size_t)f.gcount());
    return size == 0 || !f.bad();
}

inline bool WriteAllText(const std::wstring& path, const std::string& text) {
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    if (!text.empty()) f.write(text.data(), (std::streamsize)text.size());
    f.flush();
    return (bool)f;
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
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(ProjectFilePath(folder)), ec);
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
