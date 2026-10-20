// Game-virtual, case-insensitive resource lookup over the installed game root: loose files first,
// then the archives and extracted archive folders of Patches/, Local/ and Packs/, each directory in
// stem order, first match wins (cerebrivm function:vfs-boot-build-root-filesystem; same lookup as
// tools/game_fs.py, which stays the verifier). Archives are only read.
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "resource_fs.hpp"

namespace iberivm {

class GameFS {
public:
    explicit GameFS(std::filesystem::path root) : root_(std::move(root)) {
        if (!std::filesystem::is_directory(root_)) throw std::runtime_error("game root not found: " + root_.string());
        for (const char* folder : {"Patches", "Local", "Packs"}) {
            const auto dir = find_child(root_, folder);
            if (!dir) continue;
            // stem -> children (extracted folder before its same-stem archive)
            std::map<std::string, std::vector<std::filesystem::directory_entry>> stems;
            for (const auto& child : std::filesystem::directory_iterator(*dir)) {
                const auto ext = lower(child.path().extension().string());
                if (child.is_directory()) stems[lower(child.path().filename().string())].push_back(child);
                else if (ext == ".pak") stems[lower(child.path().stem().string())].push_back(child);
            }
            for (auto& [stem, children] : stems) {
                std::stable_sort(children.begin(), children.end(),
                                 [](const auto& a, const auto& b) { return a.is_directory() && !b.is_directory(); });
                for (const auto& child : children) {
                    Source s;
                    s.path = child.path();
                    s.is_dir = child.is_directory();
                    if (!s.is_dir) s.entries = &pak_directory(child.path().string());
                    sources_.push_back(std::move(s));
                }
            }
        }
    }

    static std::string key(std::string rel) {
        for (auto& c : rel) {
            if (c == '\\') c = '/';
            else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        }
        std::string out, part;
        auto flush = [&] {
            if (!part.empty() && part != ".") out += (out.empty() ? "" : "/") + part;
            part.clear();
        };
        for (char c : rel) {
            if (c == '/') flush();
            else part += c;
        }
        flush();
        return out;
    }

    bool exists(const std::string& rel) const { return locate(key(rel)).has_value(); }

    std::vector<std::uint8_t> read(const std::string& rel) const {
        const auto k = key(rel);
        const auto found = locate(k);
        if (!found) throw std::runtime_error("game resource not found: " + k);
        return read_resource(*found);
    }

    std::string read_text(const std::string& rel) const {
        const auto b = read(rel);
        return std::string(b.begin(), b.end());
    }

    // Virtual keys of the files directly inside directory `rel`, first match per name, sorted.
    std::vector<std::string> list_files(const std::string& rel) const {
        const auto k = key(rel);
        const std::string prefix = k.empty() ? "" : k + "/";
        std::map<std::string, std::string> names;
        std::vector<std::filesystem::path> bases{root_};
        for (const auto& s : sources_)
            if (s.is_dir) bases.push_back(s.path);
        for (const auto& base : bases) {
            const auto folder = resolve_ci(base, k);
            if (!folder || !std::filesystem::is_directory(*folder)) continue;
            for (const auto& child : std::filesystem::directory_iterator(*folder))
                if (child.is_regular_file()) {
                    const auto name = key(child.path().filename().string());
                    names.emplace(name, prefix + name);
                }
        }
        for (const auto& s : sources_) {
            if (s.is_dir) continue;
            for (const auto& [name, entry] : *s.entries) {
                (void)entry;
                if (name.rfind(prefix, 0) == 0 && name.find('/', prefix.size()) == std::string::npos)
                    names.emplace(name.substr(prefix.size()), name);
            }
        }
        std::vector<std::string> out;
        for (const auto& [n, full] : names) out.push_back(full);
        return out;
    }

    // Immediate virtual directories, merging loose/extracted and existing PAK directory indexes.
    // ScanAIProfiles004433B0 accepts VFS directory entries; no new archive decoder.
    std::vector<std::string> list_directories(const std::string& rel) const {
        const auto k=key(rel);const auto prefix=k.empty() ? std::string{} : k+"/";
        std::set<std::string> names;std::vector<std::filesystem::path> bases{root_};
        for(const auto& s:sources_)if(s.is_dir)bases.push_back(s.path);
        for(const auto& base:bases)if(const auto dir=resolve_ci(base,k);dir&&std::filesystem::is_directory(*dir))
            for(const auto& child:std::filesystem::directory_iterator(*dir))if(child.is_directory())names.insert(key(child.path().filename().string()));
        for(const auto& s:sources_)if(!s.is_dir)for(const auto& [name,entry]:*s.entries) {
            (void)entry;if(name.rfind(prefix,0)!=0)continue;const auto slash=name.find('/',prefix.size());
            if(slash!=std::string::npos)names.insert(name.substr(prefix.size(),slash-prefix.size()));
        }
        std::vector<std::string> result;for(const auto& name:names)result.push_back(prefix+name);return result;
    }

    // Resource spec understood by read_resource (loose path or pak:<archive>|<ENTRY>).
    std::optional<std::string> locate(const std::string& k) const {
        if (auto loose = resolve_ci(root_, k); loose && std::filesystem::is_regular_file(*loose))
            return loose->string();
        for (const auto& s : sources_)
            if (s.is_dir)
                if (auto p = resolve_ci(s.path, k); p && std::filesystem::is_regular_file(*p)) return p->string();
        for (const auto& s : sources_)
            if (!s.is_dir && s.entries->count(k)) return "pak:" + s.path.string() + "|" + k;
        // Legacy extracted-layout key <DIR>/<STEM>/<ENTRY>.
        const auto slash = k.find('/');
        if (slash != std::string::npos) {
            const auto top = k.substr(0, slash);
            const auto rest = k.substr(slash + 1);
            const auto slash2 = rest.find('/');
            if (slash2 != std::string::npos && (top == "PATCHES" || top == "LOCAL" || top == "PACKS")) {
                const auto stem = rest.substr(0, slash2), entry = rest.substr(slash2 + 1);
                for (const auto& s : sources_)
                    if (!s.is_dir && key(s.path.parent_path().filename().string()) == top &&
                        key(s.path.stem().string()) == stem && s.entries->count(entry))
                        return "pak:" + s.path.string() + "|" + entry;
            }
        }
        return std::nullopt;
    }

private:
    struct Source {
        std::filesystem::path path;
        bool is_dir = false;
        const std::map<std::string, PakEntry>* entries = nullptr;
    };
    std::filesystem::path root_;
    std::vector<Source> sources_;

    static std::string lower(std::string s) {
        for (auto& c : s)
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        return s;
    }
    static std::optional<std::filesystem::path> find_child(const std::filesystem::path& dir, const std::string& name) {
        std::error_code ec;
        const auto direct = dir / name;
        if (std::filesystem::exists(direct, ec)) return direct;  // case-insensitive file systems
        if (!std::filesystem::is_directory(dir, ec)) return std::nullopt;
        const auto want = key(name);
        for (const auto& child : std::filesystem::directory_iterator(dir, ec))
            if (key(child.path().filename().string()) == want) return child.path();
        return std::nullopt;
    }
    static std::optional<std::filesystem::path> resolve_ci(const std::filesystem::path& base, const std::string& k) {
        std::filesystem::path cur = base;
        std::size_t pos = 0;
        while (pos <= k.size() && !k.empty()) {
            const auto next = k.find('/', pos);
            const auto part = k.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
            auto child = find_child(cur, part);
            if (!child) return std::nullopt;
            cur = *child;
            if (next == std::string::npos) break;
            pos = next + 1;
        }
        return cur;
    }
};

}  // namespace iberivm
