// Read game resources named by manifest specs: a loose path, or
// "pak:<archive path>|<ENTRY/NAME>" for an entry of an HMMSYS .pak archive.
// Archive layout: cerebrivm format:pak-archive and
// claim:pak-archive-entry-offsets-are-absolute-and-header-0x24-is-directory-length.
// Entry names are matched case-insensitively with '/' separators.
#pragma once
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iberivm {

struct PakEntry { std::uint64_t offset = 0, size = 0; };

inline std::string normalize_pak_name(std::string name) {
    for (auto& c : name) {
        if (c == '\\') c = '/';
        else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i)
        if (!(name[i] == '/' && (out.empty() || out.back() == '/'))) out += name[i];
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

inline const std::map<std::string, PakEntry>& pak_directory(const std::string& archive) {
    static std::mutex lock;
    static std::map<std::string, std::map<std::string, PakEntry>> cache;
    std::lock_guard<std::mutex> guard(lock);
    const auto found = cache.find(archive);
    if (found != cache.end()) return found->second;
    std::ifstream in(archive, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open PAK archive: " + archive);
    const auto file_size = static_cast<std::uint64_t>(in.tellg());
    in.seekg(0);
    std::vector<std::uint8_t> header(0x28);
    static const char magic[] = "HMMSYS PackFile\n\x1a";  // plus terminating zero: 18 bytes
    if (!in.read(reinterpret_cast<char*>(header.data()), 0x28) ||
        !std::equal(magic, magic + 18, header.begin(), [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }))
        throw std::runtime_error("not an HMMSYS PackFile: " + archive);
    auto u32 = [](const std::vector<std::uint8_t>& b, std::size_t p) {
        if (p + 4 > b.size()) throw std::runtime_error("truncated PAK directory");
        return static_cast<std::uint32_t>(b[p]) | (static_cast<std::uint32_t>(b[p + 1]) << 8) |
               (static_cast<std::uint32_t>(b[p + 2]) << 16) | (static_cast<std::uint32_t>(b[p + 3]) << 24);
    };
    const auto count = u32(header, 0x20), directory_bytes = u32(header, 0x24);
    if (0x28ull + directory_bytes > file_size) throw std::runtime_error("PAK directory outside archive: " + archive);
    std::vector<std::uint8_t> dir(directory_bytes);
    if (!in.read(reinterpret_cast<char*>(dir.data()), static_cast<std::streamsize>(dir.size())))
        throw std::runtime_error("truncated PAK directory: " + archive);
    std::map<std::string, PakEntry> entries;
    std::string name;
    std::size_t pos = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (pos + 2 > dir.size()) throw std::runtime_error("truncated PAK directory entry: " + archive);
        const unsigned total = dir[pos], prefix = dir[pos + 1];
        pos += 2;
        if (prefix > name.size() || total < prefix || pos + (total - prefix) + 8 > dir.size())
            throw std::runtime_error("invalid PAK directory entry: " + archive);
        name = name.substr(0, prefix) + std::string(dir.begin() + pos, dir.begin() + pos + (total - prefix));
        pos += total - prefix;
        const PakEntry entry{u32(dir, pos), u32(dir, pos + 4)};
        pos += 8;
        if (entry.offset + entry.size > file_size) throw std::runtime_error("PAK entry outside archive: " + archive + "|" + name);
        entries[normalize_pak_name(name)] = entry;
    }
    if (pos != dir.size()) throw std::runtime_error("PAK directory length mismatch: " + archive);
    return cache.emplace(archive, std::move(entries)).first->second;
}

// Returns {file to open, base offset, size}; size is the whole file for loose resources.
inline std::pair<std::string, PakEntry> locate_resource(const std::string& spec) {
    if (spec.rfind("pak:", 0) == 0) {
        const auto bar = spec.find('|');
        if (bar == std::string::npos) throw std::runtime_error("invalid PAK resource spec: " + spec);
        const auto archive = spec.substr(4, bar - 4);
        const auto& entries = pak_directory(archive);
        const auto found = entries.find(normalize_pak_name(spec.substr(bar + 1)));
        if (found == entries.end()) throw std::runtime_error("PAK entry not found: " + spec);
        return {archive, found->second};
    }
    std::ifstream in(spec, std::ios::binary | std::ios::ate);
    if (!in) throw std::runtime_error("cannot open resource: " + spec);
    return {spec, PakEntry{0, static_cast<std::uint64_t>(in.tellg())}};
}

inline std::vector<std::uint8_t> read_resource_range(const std::string& spec, std::uint64_t offset, std::uint64_t size) {
    const auto [file, entry] = locate_resource(spec);
    if (offset > entry.size || size > entry.size - offset)
        throw std::runtime_error("resource range outside " + spec);
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open resource: " + file);
    in.seekg(static_cast<std::streamoff>(entry.offset + offset));
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (size && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
        throw std::runtime_error("truncated resource: " + spec);
    return data;
}

inline std::uint64_t resource_size(const std::string& spec) { return locate_resource(spec).second.size; }

inline std::vector<std::uint8_t> read_resource(const std::string& spec) {
    return read_resource_range(spec, 0, resource_size(spec));
}

}  // namespace iberivm
