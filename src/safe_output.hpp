// Stage complete bytes beside the destination before publishing the file.
#pragma once
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace mapx::app {
inline bool write_file_atomic(const std::filesystem::path& destination,
                              const std::vector<std::uint8_t>& bytes, bool replace = true) {
    static std::atomic<unsigned long> serial{0};
    std::filesystem::path staging;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 32; ++attempt) {
        staging = destination.parent_path() / (L".mapx-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(serial++) + L".tmp");
        file = CreateFileW(staging.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD error = ERROR_SUCCESS;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(file, bytes.data() + offset, count, &written, nullptr) || written != count) {
            error = GetLastError();
            if (!error) error = ERROR_WRITE_FAULT;
            break;
        }
        offset += written;
    }
    if (!error && !FlushFileBuffers(file)) error = GetLastError();
    if (!CloseHandle(file) && !error) error = GetLastError();
    if (!error && !MoveFileExW(staging.c_str(), destination.c_str(),
            MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0))) error = GetLastError();
    if (error) { DeleteFileW(staging.c_str()); SetLastError(error); return false; }
    return true;
}
} // namespace mapx::app
