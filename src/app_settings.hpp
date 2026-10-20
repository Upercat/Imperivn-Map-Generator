// Window settings in the user's local application data directory, legacy portable settings, finding the game folder, and the game's
// language (for the template texts written into the map).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>

#include <cwctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace mapx::app {

namespace fs = std::filesystem;

inline fs::path exe_dir() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return fs::path(buf).parent_path();
}

inline fs::path ini_path() {
    PWSTR folder = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder))) {
        const auto path = fs::path(folder) / L"ImperivmMapExtractor" / L"settings.ini";
        CoTaskMemFree(folder);
        return path;
    }
    return exe_dir() / L"imperivm-map-extractor.ini";
}

inline std::wstring ini_read(const wchar_t* key, const wchar_t* fallback = L"") {
    std::wstring buf(1024, L'\0');
    const DWORD n = GetPrivateProfileStringW(L"Settings", key, fallback, buf.data(), static_cast<DWORD>(buf.size()), ini_path().c_str());
    buf.resize(n);
    if (!n) {
        // Read older portable preferences without requiring write access beside the executable.
        buf.resize(1024);
        const DWORD legacy = GetPrivateProfileStringW(L"Settings", key, fallback, buf.data(),
            static_cast<DWORD>(buf.size()), (exe_dir() / L"imperivm-map-extractor.ini").c_str());
        buf.resize(legacy);
    }
    return buf;
}

inline void ini_write(const wchar_t* key, const std::wstring& value) {
    const auto path = ini_path();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (!ec) WritePrivateProfileStringW(L"Settings", key, value.c_str(), path.c_str());
}

// An Imperivm installation: the editor's random-map template volume, and the constants either loose or in the data
// pack (the reader looks in both; the workstation's copies have them loose, a Steam install may not).
inline bool looks_like_game(const fs::path& dir) {
    std::error_code ec;
    return !dir.empty() && fs::exists(dir / L"Packs" / L"randommap.BFHP", ec) &&
        (fs::exists(dir / L"DATA" / L"CONST.INI", ec) || fs::exists(dir / L"Packs" / L"data.pak", ec));
}

namespace detail {

inline std::optional<std::wstring> registry_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return std::nullopt;
    return std::wstring(buf);
}

// Library folders listed in Steam's libraryfolders.vdf (`"path"  "D:\\SteamLibrary"` lines).
inline std::vector<fs::path> parse_steam_libraries(std::istream& f, const fs::path& steam) {
    std::vector<fs::path> out{steam};
    std::string line;
    while (std::getline(f, line)) {
        const auto key = line.find("\"path\"");
        if (key == std::string::npos) continue;
        const auto open = line.find('"', key + 6);
        const auto close = open == std::string::npos ? open : line.find('"', open + 1);
        if (close == std::string::npos) continue;
        std::string p = line.substr(open + 1, close - open - 1), clean;
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
            clean += p[i];
        }
        const int length = static_cast<int>(clean.size());
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, clean.data(), length, nullptr, 0);
        if (n <= 0) continue;
        std::wstring w(static_cast<std::size_t>(n), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, clean.data(), length, w.data(), n) == n)
            out.emplace_back(w);
    }
    return out;
}

inline std::vector<fs::path> steam_libraries(const fs::path& steam) {
    std::ifstream f(steam / L"steamapps" / L"libraryfolders.vdf", std::ios::binary);
    return parse_steam_libraries(f, steam);
}

}  // namespace detail

// The game folder to start with: the saved one, the folder of the executable or its parent (the tool copied into
// or next to the game), or the Steam installation.
inline fs::path detect_game_folder() {
    if (const fs::path saved = ini_read(L"GameFolder"); looks_like_game(saved)) return saved;
    const auto here = exe_dir();
    for (const auto& dir : {here, here.parent_path()})
        if (looks_like_game(dir)) return dir;
    for (const auto root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
        auto steam = detail::registry_string(root, L"Software\\Valve\\Steam", L"SteamPath");
        if (!steam) steam = detail::registry_string(root, L"Software\\WOW6432Node\\Valve\\Steam", L"InstallPath");
        if (!steam) continue;
        for (const auto& lib : detail::steam_libraries(*steam)) {
            const auto dir = lib / L"steamapps" / L"common" / L"Imperivm - Great Battles of Rome HD";
            if (looks_like_game(dir)) return dir;
        }
    }
    return {};
}

// The game's language pack name ("spanish") from Settings.ini [Language] Default, if that pack is installed.
inline std::string game_text_language(const fs::path& game) {
    std::wstring buf(64, L'\0');
    const DWORD n = GetPrivateProfileStringW(L"Language", L"Default", L"", buf.data(), static_cast<DWORD>(buf.size()),
                                             (game / L"Settings.ini").c_str());
    buf.resize(n);
    std::string lang;
    for (const wchar_t c : buf)
        if (c < 128) lang += static_cast<char>(std::towlower(c));
    std::error_code ec;
    for (const wchar_t* folder : {L"local", L"Local"})
        if (!lang.empty() && fs::exists(game / folder / fs::path(lang + ".pak"), ec)) return lang;
    return "english";
}

}  // namespace mapx::app
