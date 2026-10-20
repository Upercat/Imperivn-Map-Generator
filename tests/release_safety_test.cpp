// Failure-oriented checks of exported files and Steam paths; no game data required.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <thread>
#include "app_settings.hpp"
#include "safe_output.hpp"
int main() {
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* name) { if (!ok) { ++failures; std::cerr << "FAIL " << name << '\n'; } };
    const auto temporary = fs::weakly_canonical(fs::temp_directory_path());
    const auto root = temporary / (L"mapx-release-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(root);
    const auto destination = root / L"mapa-ñ.BFHP";
    auto read = [&](const fs::path& path) {
        std::ifstream f(path, std::ios::binary);
        return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), {});
    };
    const std::vector<std::uint8_t> first{1,2,3}, second{4,5,6};
    check(mapx::app::write_file_atomic(destination, first), "first write");
    check(!mapx::app::write_file_atomic(destination, second, false), "no replacement");
    check(read(destination) == first, "existing file retained");
    HANDLE locked = CreateFileW(destination.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    check(locked != INVALID_HANDLE_VALUE, "lock fixture");
    check(!mapx::app::write_file_atomic(destination, second), "locked file rejects write");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    check(read(destination) == first, "failed replacement retained original");
    check(mapx::app::write_file_atomic(destination, second) && read(destination) == second, "complete replacement");
    check(!mapx::app::write_file_atomic(root / L"missing" / L"map.BFHP", first), "missing folder rejects write");
    const auto race = root / L"race.BFHP";
    bool a = false, b = false;
    std::thread one([&] { a = mapx::app::write_file_atomic(race, first, false); });
    std::thread two([&] { b = mapx::app::write_file_atomic(race, second, false); });
    one.join(); two.join();
    check(a != b && read(race) == (a ? first : second), "concurrent no-replace has one intact winner");
    std::size_t leftovers = 0;
    for (const auto& entry : fs::directory_iterator(root)) if (entry.path().extension() == L".tmp") ++leftovers;
    check(leftovers == 0, "staging cleanup");
    std::istringstream vdf("\"path\" \"D:\\\\Steam Library\"\n\"path\" \"E:\\\\Bibliot\xc3\xa9" "ca\"\n");
    const auto libraries = mapx::app::detail::parse_steam_libraries(vdf, L"C:\\Steam");
    check(libraries.size() == 3 && libraries[1] == L"D:\\Steam Library" &&
          libraries[2] == L"E:\\Bibliotéca", "Unicode Steam library paths");
    std::istringstream invalid("\"path\" \"\xff\"\n");
    check(mapx::app::detail::parse_steam_libraries(invalid, L"C:\\Steam").size() == 1, "invalid UTF-8 omitted");
    const auto settings = mapx::app::ini_path();
    check(settings.filename() == L"settings.ini" && settings.parent_path().filename() == L"ImperivmMapExtractor",
          "per-user preferences outside executable folder");
    const auto resolved = fs::weakly_canonical(root);
    if (resolved.parent_path() == temporary && resolved.filename() == root.filename()) fs::remove_all(root);
    else check(false, "temporary cleanup path escaped its expected directory");
    std::cout << (failures ? "FAILED" : "PASS release safety") << '\n';
    return failures ? 1 : 0;
}
