// Class properties of DATA/CLASSES as the random-map reference reads them (objects.class_props,
// objects._heir, objects._cpp_class, objects._class_files): each class file is keyed by the id and
// altid of its first <class> tag; properties are every <properties .../> of that file, merged up
// the parent chain with the child winning.
#pragma once
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include "game_fs.hpp"
#include "randommap_data.hpp"
#include "xml_lite.hpp"

namespace iberivm::rmdata {

class ClassProps {
public:
    explicit ClassProps(const GameFS& fs) : fs_(fs) {}

    using Props = std::vector<std::pair<std::string, std::string>>;  // first occurrence wins, in order

    const Props& props(const std::string& cls) {
        const auto key = lower(cls);
        if (const auto it = props_.find(key); it != props_.end()) return it->second;
        Props out;
        std::vector<std::string> seen;
        std::string c = key;
        while (!c.empty() && std::find(seen.begin(), seen.end(), c) == seen.end()) {
            seen.push_back(c);
            const auto& files = class_files();
            const auto it = files.find(c);
            if (it == files.end() || it->second.empty()) break;
            const auto& t = it->second.front();
            for (const auto& body : xml::self_closing_bodies(t, "properties"))
                for (const auto& [k, v] : xml::regex_attrs(body))
                    if (std::none_of(out.begin(), out.end(), [&](const auto& p) { return p.first == k; })) out.emplace_back(k, v);
            c = lower(first_class_attr(t, "parent").value_or(""));
        }
        return props_.emplace(key, std::move(out)).first->second;
    }

    std::optional<std::string> prop(const std::string& cls, const std::string& name) {
        for (const auto& [k, v] : props(cls))
            if (k == name) return v;
        return std::nullopt;
    }

    bool heir(const std::string& cls, const std::string& base) {
        std::vector<std::string> seen;
        std::string c = lower(cls);
        const auto b = lower(base);
        while (!c.empty() && std::find(seen.begin(), seen.end(), c) == seen.end()) {
            if (c == b) return true;
            seen.push_back(c);
            const auto& files = class_files();
            const auto it = files.find(c);
            if (it == files.end() || it->second.empty()) return false;
            c = lower(first_class_attr(it->second.front(), "parent").value_or(""));
        }
        return false;
    }

    std::optional<std::string> cpp_class(const std::string& cls) {
        std::vector<std::string> seen;
        std::string c = lower(cls);
        while (!c.empty() && std::find(seen.begin(), seen.end(), c) == seen.end()) {
            seen.push_back(c);
            const auto& files = class_files();
            const auto it = files.find(c);
            if (it == files.end() || it->second.empty()) return std::nullopt;
            const auto& t = it->second.front();
            if (auto cc = first_class_attr(t, "cpp_class"); cc && !cc->empty()) return cc;
            c = lower(first_class_attr(t, "parent").value_or(""));
        }
        return std::nullopt;
    }

private:
    const GameFS& fs_;
    std::map<std::string, std::vector<std::string>> files_;
    bool loaded_ = false;
    std::map<std::string, Props> props_;

    static std::size_t find_class_tag(const std::string& t) {
        std::size_t p = 0;
        for (;;) {
            const auto b = t.find("<class", p);
            if (b == std::string::npos || b + 6 >= t.size()) return std::string::npos;
            const char c = t[b + 6];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') return b;
            p = b + 1;
        }
    }
    static std::optional<std::string> first_class_attr(const std::string& t, const std::string& name) {
        const auto b = find_class_tag(t);
        if (b == std::string::npos) return std::nullopt;
        const auto e = t.find('>', b + 7);
        if (e == std::string::npos) return std::nullopt;
        const auto a = xml::regex_attrs(t.substr(b + 7, e - b - 7));
        if (const auto* v = xml::regex_get(a, name)) return *v;
        return std::nullopt;
    }
    const std::map<std::string, std::vector<std::string>>& class_files() {
        if (loaded_) return files_;
        loaded_ = true;
        for (const auto& f : fs_.list_files("DATA/CLASSES")) {
            auto t = fs_.read_text(f);
            const auto b = find_class_tag(t);
            if (b == std::string::npos) continue;
            const auto e = t.find('>', b + 7);
            if (e == std::string::npos) continue;
            const auto a = xml::regex_attrs(t.substr(b + 7, e - b - 7));
            for (const char* k : {"id", "altid"})
                if (const auto* key = xml::regex_get(a, k); key && !key->empty()) files_[lower(*key)].push_back(t);
        }
        return files_;
    }
};

}  // namespace iberivm::rmdata
