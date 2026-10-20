// W4 map assembly: the generated map as the files of an editor-saved .BFHP, and the packed volume. A port of
// Iberivm tools/randommap/build_map.py (write, pack, map_template_translations, localise); keep the two identical.
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "generate.hpp"
#include "hpfs_writer.hpp"
#include "resource_fs.hpp"

namespace mapx {

// Paths inside the volume in the order the reference builds them; setting an existing path keeps its place.
class OrderedFiles {
public:
    void set(const std::string& path, hpfs::Bytes data) {
        for (auto& [p, d] : items_)
            if (p == path) {
                d = std::move(data);
                return;
            }
        items_.emplace_back(path, std::move(data));
    }
    const hpfs::Bytes* find(const std::string& path) const {
        for (const auto& [p, d] : items_)
            if (p == path) return &d;
        return nullptr;
    }
    const std::vector<std::pair<std::string, hpfs::Bytes>>& items() const { return items_; }

private:
    std::vector<std::pair<std::string, hpfs::Bytes>> items_;
};

inline constexpr std::array<const char*, 8> kRaceNames{"Gaul", "RepublicanRome", "Carthage", "Iberia", "ImperialRome", "Britain",
                                                      "Egypt", "Germany"};

namespace detail {

inline std::string text(const hpfs::Bytes& b) { return std::string(b.begin(), b.end()); }  // latin-1, byte for byte
inline hpfs::Bytes bytes(const std::string& s) { return hpfs::Bytes(s.begin(), s.end()); }

// re.sub with a replacement function, every non-overlapping match left to right.
inline std::string sub(const std::string& s, const std::regex& re, const std::function<std::string(const std::smatch&)>& fn) {
    std::string out;
    auto begin = s.cbegin();
    std::smatch m;
    while (std::regex_search(begin, s.cend(), m, re)) {
        out.append(begin, m[0].first);
        out += fn(m);
        begin = m[0].second;
        if (m[0].length() == 0) {
            if (begin == s.cend()) break;
            out += *begin++;
        }
    }
    out.append(begin, s.cend());
    return out;
}

inline std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    std::string out;
    std::size_t pos = 0;
    for (auto at = s.find(from); at != std::string::npos; at = s.find(from, pos)) {
        out.append(s, pos, at - pos);
        out += to;
        pos = at + from.size();
    }
    out.append(s, pos, std::string::npos);
    return out;
}

inline std::string regex_escape(const std::string& s) {
    std::string out;
    for (const char c : s) {
        if (std::string("\\^$.|?*+()[]{}/").find(c) != std::string::npos) out += '\\';
        out += c;
    }
    return out;
}

}  // namespace detail

// Editor template texts are translated with context "map template" from local/<lang>.pak CURRENTLANG/GAME.LOC.XML;
// English keeps the template text. Pairs in first-appearance order, later duplicates overriding the value.
inline std::vector<std::pair<std::string, std::string>> map_template_translations(const std::filesystem::path& game_root,
                                                                                 const std::string& lang) {
    std::vector<std::pair<std::string, std::string>> tr;
    if (lang.empty() || detail::lower_ascii(lang) == "english") return tr;
    std::filesystem::path pak;
    for (const char* folder : {"local", "Local", "LOCAL"})
        if (std::filesystem::exists(game_root / folder / (lang + ".pak"))) pak = game_root / folder / (lang + ".pak");
    if (pak.empty()) throw std::runtime_error("no language pack " + lang);
    const auto loc = iberivm::read_resource("pak:" + pak.string() + "|CURRENTLANG/GAME.LOC.XML");
    const std::string t(loc.begin(), loc.end());
    static const std::regex entry(R"re(justtext="([^"]*)" context="map template" result="([^"]*)")re");
    for (auto it = std::sregex_iterator(t.begin(), t.end(), entry); it != std::sregex_iterator(); ++it) {
        const auto src = (*it)[1].str(), dst = (*it)[2].str();
        bool found = false;
        for (auto& [s, d] : tr)
            if (s == src) d = dst, found = true;
        if (!found) tr.emplace_back(src, dst);
    }
    return tr;
}

inline std::string localise(std::string text, const std::vector<std::pair<std::string, std::string>>& tr) {
    for (const auto& [src, dst] : tr) {
        if (src.find("%d1") != std::string::npos) {
            const std::regex re("\"" + detail::replace_all(detail::regex_escape(src), "%d1", "(\\d+)") + "\"");
            text = detail::sub(text, re, [&](const std::smatch& m) { return "\"" + detail::replace_all(dst, "%d1", m[1].str()) + "\""; });
        } else {
            text = detail::replace_all(text, "\"" + src + "\"", "\"" + dst + "\"");
        }
    }
    return text;
}

// Player start of each slot: the last BaseTownhall member it owns, at (x, trunc(y * 181 / 256) - height(x, y))
// (0x004AFF30), from the generator's object list and final grids.
inline std::map<int, std::pair<long long, long long>> player_starts(Install& install, const MapFiles& map, int size) {
    auto payload = [&](const char* name, int rows) {
        const auto& f = map.files.at(name);
        iberivm::rm::Grid<std::uint8_t> g(rows, rows, 0);
        if (f.size() != 20 + g.v.size()) throw std::runtime_error(std::string("unexpected size of ") + name);
        std::copy(f.begin() + 20, f.end(), g.v.begin());
        return g;
    };
    const auto heights = payload("Terrain.height.grid", size >> 5);
    const auto classes = payload("Terrain.terrain.grid", ((size - 1) >> 6) + 1);
    const iberivm::rm::Passability pa(size, heights, classes);
    std::map<int, std::pair<long long, long long>> starts;
    std::istringstream lines(map.objects);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream f(line);
        std::string kind, cls, facing;
        long long x = 0, y = 0;
        int owner = 99;
        if (!(f >> kind >> cls >> x >> y >> facing >> owner) || kind != "member" || owner < 0 || owner >= 16) continue;
        // the list writes spaces in class names as '_'
        if (!install.props().heir(cls, "BaseTownhall") && !install.props().heir(detail::replace_all(cls, "_", " "), "BaseTownhall")) continue;
        starts[owner] = {x, ((y * 181) >> 8) - pa.height(x, y)};
    }
    return starts;
}

// Every file of the saved map, keyed by its path inside the BFHP, in the reference's order (build_map.write).
inline OrderedFiles assemble_map(Install& install, const MapFiles& map, const InputFile& in, const PreSession& pre,
                                 const std::string& lang) {
    const auto tr = map_template_translations(install.root(), lang);
    const int size = static_cast<int>(in.settings.random_map.map_size);
    OrderedFiles files;
    for (auto& [path, data] : hpfs::flatten(hpfs::read_tree(install.fs().read("Packs/randommap.BFHP")))) files.set(path, data);
    const std::string m = "Maps/1/";
    files.set("itemsCustom.xml", detail::bytes("\t<items>\r\n\t</items>\r\n"));
    // editor env block, constant in every captured editor save
    files.set("env.42", {0x4c, 0x5a, 0x49, 0x53, 0x04, 0, 0, 0, 0, 0, 0x02, 0, 0x02, 0xf0, 0x12, 0, 0, 0, 0, 0, 0, 0});
    for (const char* grid : {"terrain", "height", "light", "trans", "decor", "pass"}) {
        const std::string name = std::string("Terrain.") + grid + ".grid";
        files.set(m + name, map.files.at(name));
    }
    files.set(m + "map.obj.xml", map.files.at("map.obj.xml"));

    const auto starts = player_starts(install, map, size);
    auto start = [&](int k) { const auto it = starts.find(k); return it == starts.end() ? std::pair<long long, long long>{0, 0} : it->second; };
    static const std::regex race_re(R"re(race="[^"]*")re"), control_re(R"re(control="[^"]*")re"),
        startx_re(R"re(startx="[^"]*")re"), starty_re(R"re(starty="[^"]*")re"), season_re(R"re(season="[^"]*")re"),
        size_re(R"re((<size\s+x=")\d+("\s+y=")\d+")re"), start_pt_re(R"re((<start_pt\s+x=")-?\d+("\s+y=")-?\d+")re");
    const auto& teams = in.settings.random_map.teams;
    for (int k = 0; k < 16; ++k) {
        const std::string name = "player" + std::to_string(k) + ".xml";
        const auto* f = files.find(name);
        if (!f) throw std::runtime_error("the editor template has no " + name);
        std::string t = detail::text(*f);
        if (k < 8) {
            const bool active = teams[static_cast<std::size_t>(k)] != kNoTeam;
            const int race = pre.races[static_cast<std::size_t>(k)];
            if (active && (race < 0 || race > 7)) throw std::runtime_error("slot " + std::to_string(k) + " without a resolved race");
            const std::string race_name = active ? kRaceNames[static_cast<std::size_t>(race)] : "Random";
            t = detail::sub(t, race_re, [&](const std::smatch&) { return "race=\"" + race_name + "\""; });
            t = detail::sub(t, control_re, [&](const std::smatch&) { return std::string("control=\"") + (active ? "Both" : "Disabled") + "\""; });
        }
        const auto [sx, sy] = start(k);
        t = detail::sub(t, startx_re, [&](const std::smatch&) { return "startx=\"" + std::to_string(sx) + "\""; });
        t = detail::sub(t, starty_re, [&](const std::smatch&) { return "starty=\"" + std::to_string(sy) + "\""; });
        files.set(name, detail::bytes(localise(t, tr)));
    }
    // a game start (replay, skirmish) is played in spring: the lobby overwrites the map's season
    std::string game = detail::sub(detail::text(*files.find("game.xml")), season_re, [](const std::smatch&) { return std::string("season=\"spring\""); });
    files.set("game.xml", detail::bytes(localise(game, tr)));
    std::string mapxml = detail::text(*files.find(m + "map.xml"));
    mapxml = detail::sub(mapxml, size_re, [&](const std::smatch& s) { return s[1].str() + std::to_string(size) + s[2].str() + std::to_string(size) + "\""; });
    const auto [sx0, sy0] = start(0);
    mapxml = detail::sub(mapxml, start_pt_re, [&](const std::smatch& s) { return s[1].str() + std::to_string(sx0) + s[2].str() + std::to_string(sy0) + "\""; });
    files.set(m + "map.xml", detail::bytes(localise(mapxml, tr)));
    return files;
}

// The files as a loadable .BFHP: the editor template's tree (empty directories included) with every file replaced.
// The editor save rewrites player0.xml..player15.xml (in numeric order) and then env.42, so they end the root
// directory of every captured save (build_map.pack).
inline hpfs::Bytes pack_map(Install& install, const OrderedFiles& files) {
    auto tree = hpfs::read_tree(install.fs().read("Packs/randommap.BFHP"));
    std::vector<std::string> tail;
    for (int k = 0; k < 16; ++k) tail.push_back("player" + std::to_string(k) + ".xml");
    tail.push_back("env.42");
    for (const auto& name : tail) hpfs::remove_child(tree, name);
    auto in_tail = [&](const std::string& p) { return std::find(tail.begin(), tail.end(), p) != tail.end(); };
    for (const auto& [path, data] : files.items())
        if (!in_tail(path)) hpfs::put_file(tree, path, data);
    for (const auto& name : tail)
        if (const auto* d = files.find(name)) hpfs::put_file(tree, name, *d);
    auto vol = hpfs::write_volume(tree);
    // the volume must read back to exactly these files and pass the structural check
    if (const auto problems = hpfs::validate(vol); !problems.empty()) throw std::runtime_error("packed volume: " + problems.front());
    auto back = hpfs::flatten(hpfs::read_tree(vol));
    if (back.size() != files.items().size()) throw std::runtime_error("packed volume: file count differs");
    for (const auto& [path, data] : back) {
        const auto* want = files.find(path);
        if (!want || *want != data) throw std::runtime_error("packed volume: " + path + " differs");
    }
    return vol;
}

}  // namespace mapx
