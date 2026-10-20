// Rule files of the random-map generator (reference: exact.decor_registry, exact.tree_catalogue,
// placeobjects.parse_descriptors, terrain_classes.native_tables): DECORS.INI decor registry,
// PlaceTrees.xml tree catalogue, PlaceObjects.xml descriptors and TerrainMix.xml tables.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include "game_fs.hpp"
#include "randommap_data.hpp"
#include "xml_lite.hpp"

namespace iberivm::rmdata {

inline std::string strip(const std::string& s) {
    std::size_t b = 0, e = s.size();
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (b < e && sp(s[b])) ++b;
    while (e > b && sp(s[e - 1])) --e;
    return s.substr(b, e - b);
}

inline std::vector<std::string> split_lines(const std::string& t) {
    // str.splitlines over \r\n, \r and \n
    std::vector<std::string> out;
    std::string cur;
    for (std::size_t i = 0; i < t.size(); ++i) {
        const char c = t[i];
        if (c == '\r' || c == '\n') {
            out.push_back(cur);
            cur.clear();
            if (c == '\r' && i + 1 < t.size() && t[i + 1] == '\n') ++i;
        } else
            cur += c;
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// INI sections as (name, key -> value) with keys lower-cased, values stripped, ';' comments cut
// (the parsing of exact.decor_registry and passability.decor_entities).
inline std::vector<std::pair<std::string, std::map<std::string, std::string>>> ini_sections(const std::string& text) {
    std::vector<std::pair<std::string, std::map<std::string, std::string>>> out;
    for (const auto& raw : split_lines(text)) {
        const auto t = strip(raw);
        // ^\[(.+?)\]\s*$
        if (!raw.empty() && raw[0] == '[' && t.size() >= 3 && t.back() == ']') {
            out.emplace_back(t.substr(1, t.size() - 2), std::map<std::string, std::string>{});
            continue;
        }
        if (out.empty()) continue;
        auto line = strip(raw.substr(0, raw.find(';')));
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto key = lower(strip(line.substr(0, eq)));
        out.back().second[key] = strip(line.substr(eq + 1));
    }
    return out;
}

// 0x005441D0 over DECORS.INI: per type id the SubGroup and rnd_map_use
// (claim:decor-registry-count-is-max-type-id).
struct DecorRegistry {
    struct Entry {
        bool present = false;
        std::string subgroup;
        bool rnd = false;
    };
    std::vector<Entry> reg = std::vector<Entry>(256);
    int top = 0;
    std::map<int, std::string> entity;  // passability.decor_entities: first entity per type
};

inline DecorRegistry load_decor_registry(const GameFS& fs) {
    DecorRegistry r;
    const auto text = fs.read_text("MapObjects/DECORS/DECORS.INI");
    for (const auto& [name, kv] : ini_sections(text)) {
        (void)name;
        const auto t = kv.find("type");
        const long long tid = t == kv.end() ? 0 : py_int(t->second);
        if (0 < tid && tid < 0x100) {
            auto& e = r.reg[static_cast<std::size_t>(tid)];
            e.present = true;
            const auto sg = kv.find("subgroup");
            e.subgroup = sg == kv.end() ? "" : sg->second;
            const auto rnd = kv.find("rnd_map_use");
            e.rnd = (rnd == kv.end() ? std::string("0") : rnd->second) != "0";
        }
        if (t != kv.end() && kv.count("entity")) r.entity.emplace(static_cast<int>(tid), kv.at("entity"));
    }
    for (int i = 0; i < 256; ++i)
        if (r.reg[static_cast<std::size_t>(i)].present) r.top = i;
    return r;
}

// 0x00582C40 TreeRace/TreeGroup/TreeSubGrp (claim:random-map-tree-catalogue-parse-drops-zero-weight-and-empty-entries).
struct TreeTable {
    struct Entry {
        int w = 0, first = 0, n = 0;
    };
    std::vector<Entry> G, S;
    std::vector<int> T;
};

inline const char* const kTreeRaces[8] = {"Gaul", "RepublicanRome", "Carthage", "Iberia", "ImperialRome", "Britain", "Egypt", "Germany"};

inline std::vector<TreeTable> load_tree_catalogue(const GameFS& fs, const DecorRegistry& reg) {
    const auto root = xml::parse(fs.read_text("data/RandomMap/PlaceTrees.xml"));
    std::vector<TreeTable> tables(8);
    std::vector<const xml::Node*> races;
    root.iter("TreeRace", races);
    for (const auto* tr : races) {
        int ri = -1;
        for (int i = 0; i < 8; ++i)
            if (tr->get("race") == kTreeRaces[i]) ri = i;
        if (ri < 0) throw std::runtime_error("PlaceTrees.xml: unknown race " + tr->get("race"));
        auto& tb = tables[static_cast<std::size_t>(ri)];
        for (const auto* grp : tr->all("TreeGroup")) {
            const int gw = static_cast<int>(py_int(grp->get("weight", "100")));
            if (gw == 0) continue;
            TreeTable::Entry entry{gw, static_cast<int>(tb.S.size()), 0};
            for (const auto* sg : grp->all("TreeSubGrp")) {
                const int sw = static_cast<int>(py_int(sg->get("weight", "100")));
                if (sw == 0) continue;
                const auto* sname = sg->find_attr("name");
                std::vector<int> types;
                for (int i = 0; i < reg.top; ++i) {
                    const auto& e = reg.reg[static_cast<std::size_t>(i)];
                    if (e.present && e.rnd && sname && e.subgroup == *sname) types.push_back(i);
                }
                if (types.empty()) continue;
                tb.S.push_back({sw, static_cast<int>(tb.T.size()), static_cast<int>(types.size())});
                tb.T.insert(tb.T.end(), types.begin(), types.end());
                ++entry.n;
            }
            if (entry.n) tb.G.push_back(entry);
        }
    }
    return tables;
}

}  // namespace iberivm::rmdata
