// Readers for the random-map generator's external inputs (tools/randommap/data.py is the verifier):
// map templates (data/RandomMap/*.rm.xml), template bitmaps, class records (DATA/CLASSES), entity
// rects (entity XML plus RLE image cell rects), and the settlement library volume Maps/1, decoded from the
// shipped Packs/RandomMapSettlements.bfhp (LZIS, then HPFS) or read from an extracted copy.
#pragma once
#include <algorithm>
#include <cctype>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "game_fs.hpp"
#include "hpfs_volume.hpp"
#include "lzis.hpp"
#include "sha256.hpp"
#include "xml_lite.hpp"

namespace iberivm::rmdata {

inline std::string lower(std::string s) {
    for (auto& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

inline std::uint32_t le32(const std::vector<std::uint8_t>& b, std::size_t p) {
    if (p + 4 > b.size()) throw std::runtime_error("truncated binary data");
    return std::uint32_t(b[p]) | (std::uint32_t(b[p + 1]) << 8) | (std::uint32_t(b[p + 2]) << 16) |
           (std::uint32_t(b[p + 3]) << 24);
}

// Python int() of an attribute string (decimal, optional sign and surrounding spaces).
inline long long py_int(const std::string& s) {
    std::size_t used = 0;
    const auto v = std::stoll(s, &used, 10);
    while (used < s.size() && (s[used] == ' ' || s[used] == '\t')) ++used;
    if (used != s.size()) throw std::runtime_error("invalid integer: " + s);
    return v;
}

// 0x005CEAC0 attribute map (claim:random-map-template-xml-directives-map-to-template-fields).
struct MapTemplate {
    std::string bitmap;
    int allow_rotate = 0, allow_mirror = 0;
    double amp = 0.0;
    std::vector<std::array<int, 3>> palette;
    std::vector<std::string> meanings;
    int meaning(const std::string& m) const {
        for (std::size_t i = 0; i < meanings.size(); ++i)
            if (meanings[i] == m) return static_cast<int>(i);
        throw std::runtime_error("template has no colour meaning " + m);
    }
};

inline MapTemplate load_template(const GameFS& fs, const std::string& name) {
    const auto root = xml::parse(fs.read_text("data/RandomMap/" + name + ".rm.xml"));
    MapTemplate t;
    t.bitmap = root.child("bitmap")->get("file");
    if (const auto* tr = root.child("transform")) {
        t.allow_rotate = static_cast<int>(py_int(tr->get("allow-rotate", "0")));
        t.allow_mirror = static_cast<int>(py_int(tr->get("allow-mirror", "0")));
    }
    t.amp = std::stod(root.child("distortion")->get("amp"));
    for (const auto* c : root.all("color-code")) {
        t.palette.push_back({static_cast<int>(py_int(c->get("red"))), static_cast<int>(py_int(c->get("green"))),
                             static_cast<int>(py_int(c->get("blue")))});
        t.meanings.push_back(c->get("meaning"));
    }
    return t;
}

// 0x0062EEF0 / 0x0063D640 / 0x00647FA0: 512 x 512 16-bpp bitmap as RGB555 expanded with |3; rows in
// GetPixel order (file row 511 - y).
inline std::vector<std::array<int, 3>> decode_bmp16(const std::vector<std::uint8_t>& b) {
    const auto off = le32(b, 10);
    const auto w = static_cast<std::int32_t>(le32(b, 18)), h = static_cast<std::int32_t>(le32(b, 22));
    const unsigned bc = b.at(28) | (b.at(29) << 8);
    if (w != 512 || h != 512 || bc != 16) throw std::runtime_error("template bitmap is not 512x512x16");
    if (off + 512u * 512u * 2u > b.size()) throw std::runtime_error("truncated template bitmap");
    std::vector<std::array<int, 3>> out(512 * 512);
    for (int y = 0; y < 512; ++y)
        for (int x = 0; x < 512; ++x) {
            const std::size_t p = off + (static_cast<std::size_t>(511 - y) * 512 + x) * 2;
            const unsigned v = b[p] | (b[p + 1] << 8);
            out[y * 512 + x] = {static_cast<int>((((v >> 10) & 31) << 3) | 3), static_cast<int>((((v >> 5) & 31) << 3) | 3),
                                static_cast<int>(((v & 31) << 3) | 3)};
        }
    return out;
}

struct Rect {
    int l = 0, t = 0, r = 0, b = 0;
};

// Class records of DATA/CLASSES and entity rects (entity +0x288).
class ClassData {
public:
    explicit ClassData(const GameFS& fs) : fs_(fs) {}

    // passability.class_index: every <class ...> of every file; id and altid, first wins.
    struct Record {
        std::optional<std::string> entity, parent;
    };
    const std::map<std::string, Record>& index(const std::string& season = "spring") {
        auto& idx = index_[season];
        if (!idx.empty()) return idx;
        for (const auto& f : fs_.list_files("DATA/CLASSES")) {
            const auto t = fs_.read_text(f);
            std::size_t p = 0;
            for (;;) {
                const auto b = find_class_tag(t, p);
                if (b == std::string::npos) break;
                const auto e = t.find('>', b + 7);
                if (e == std::string::npos) break;
                const auto a = xml::regex_attrs(t.substr(b + 7, e - b - 7));
                Record rec;
                const auto* es = xml::regex_get(a, "entity_" + season);
                const auto* en = xml::regex_get(a, "entity");
                if (es && !es->empty()) rec.entity = *es;
                else if (en && !en->empty()) rec.entity = *en;
                if (const auto* pa = xml::regex_get(a, "parent")) rec.parent = *pa;
                for (const char* k : {"id", "altid"})
                    if (const auto* key = xml::regex_get(a, k); key && !key->empty()) idx.emplace(lower(*key), rec);
                p = e + 1;
            }
        }
        return idx;
    }

    // passability.class_entity
    std::optional<std::string> class_entity(const std::string& cls, const std::string& season = "spring") {
        std::vector<std::string> seen;
        auto c = lower(cls);
        const auto& idx = index(season);
        while (!c.empty() && std::find(seen.begin(), seen.end(), c) == seen.end()) {
            seen.push_back(c);
            const auto it = idx.find(c);
            if (it == idx.end()) return std::nullopt;
            if (it->second.entity) return it->second.entity;
            c = lower(it->second.parent.value_or(""));
        }
        return std::nullopt;
    }

    // entity_rect.entity_rect (LoadEntityXmlFile 0x006010B0): union of the RLE image cell rects of
    // every layer and replace element, offset by their offsetx/offsety.
    std::optional<Rect> entity_rect(const std::string& ent_path) {
        if (const auto it = rects_.find(ent_path); it != rects_.end()) return it->second;
        const auto t = fs_.read_text(ent_path);
        const auto slash = ent_path.rfind('/');
        const std::string folder = slash == std::string::npos ? ent_path : ent_path.substr(0, slash);
        std::map<long long, std::vector<std::pair<std::string, std::string>>> images;
        for (const auto& body : xml::self_closing_bodies(t, "image")) {
            const auto a = xml::regex_attrs(body);
            images[py_int(*xml::regex_get(a, "idx"))] = a;
        }
        std::optional<Rect> rect;
        auto add = [&](Rect r) {
            if (r.l > r.r || r.t > r.b) return;
            if (!rect) rect = r;
            else rect = Rect{std::min(rect->l, r.l), std::min(rect->t, r.t), std::max(rect->r, r.r), std::max(rect->b, r.b)};
        };
        auto cells = [&](long long idx) -> std::vector<Rect> {
            const auto it = images.find(idx);
            if (it == images.end()) return {};
            const auto& f = *xml::regex_get(it->second, "file");
            const auto lf = lower(f);
            const bool rle = lf.size() >= 4 && lf.compare(lf.size() - 4, 4, ".rle") == 0;
            return mmp_cells(folder + "/" + f + (rle ? ".mmp" : ".rle.mmp"));
        };
        for (const char* tag : {"layer", "replace"}) {
            for (const auto& body : xml::self_closing_bodies(t, tag)) {
                const auto a = xml::regex_attrs(body);
                const auto* ox = xml::regex_get(a, "offsetx");
                const auto* oy = xml::regex_get(a, "offsety");
                const int dx = ox ? static_cast<int>(py_int(*ox)) : 0, dy = oy ? static_cast<int>(py_int(*oy)) : 0;
                for (const auto& c : cells(py_int(*xml::regex_get(a, "image"))))
                    add(Rect{c.l + dx, c.t + dy, c.r + dx, c.b + dy});
            }
        }
        rects_[ent_path] = rect;
        return rect;
    }

    std::optional<Rect> class_rect(const std::string& cls) {
        const auto e = class_entity(cls);
        if (!e) return std::nullopt;
        return entity_rect(*e);
    }

private:
    const GameFS& fs_;
    std::map<std::string, std::map<std::string, Record>> index_;
    std::map<std::string, std::optional<Rect>> rects_;

    // regex `<class\s(.*?)>`: "<class" followed by one whitespace character.
    static std::size_t find_class_tag(const std::string& t, std::size_t p) {
        for (;;) {
            const auto b = t.find("<class", p);
            if (b == std::string::npos || b + 6 >= t.size()) return std::string::npos;
            const char c = t[b + 6];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') return b;
            p = b + 1;
        }
    }

    // CVXRLEImage::Load 0x0060C540: cell rects of an IMGRLE file (empty when missing or not RLE).
    std::vector<Rect> mmp_cells(const std::string& path) {
        if (!fs_.exists(path)) return {};
        const auto raw = fs_.read(path);
        if (raw.size() < 26 || std::string(raw.begin(), raw.begin() + 6) != "IMGRLE") return {};
        const auto cols = le32(raw, 18), rows = le32(raw, 22);
        std::size_t pos = 26;
        std::vector<Rect> out;
        for (std::uint32_t i = 0; i < cols * rows; ++i) {
            Rect r{static_cast<std::int32_t>(le32(raw, pos)), static_cast<std::int32_t>(le32(raw, pos + 4)),
                   static_cast<std::int32_t>(le32(raw, pos + 8)), static_cast<std::int32_t>(le32(raw, pos + 12))};
            pos += 16;
            out.push_back(r);
            if (r.l <= r.r && r.t <= r.b) {
                if (pos + 3 > raw.size() || raw[pos] != 'R' || raw[pos + 1] != 'L' || raw[pos + 2] != 'E')
                    throw std::runtime_error("RLE block expected in " + path);
                const char tag[] = "pamm";
                const auto it = std::search(raw.begin() + static_cast<std::ptrdiff_t>(pos), raw.end(), tag, tag + 4);
                if (it == raw.end()) throw std::runtime_error("RLE block end missing in " + path);
                pos = static_cast<std::size_t>(it - raw.begin()) + 8;
            }
        }
        return out;
    }
};

// Settlement library volume Maps/1 of Packs/RandomMapSettlements.bfhp (tools/randommap/data.py library_bytes).
// By default the shipped pack is decoded in C++ (lzis.hpp, then hpfs_volume.hpp); from_directory() reads an
// extracted copy instead (Maps/1/<name> under the directory, IBERIVM_RANDOMMAP_LIBRARY). Either way the files
// the reference verifies are checked against its SHA-256 values (data.py LIBRARY_SHA256).
class SettlementLibraryFiles {
public:
    static SettlementLibraryFiles from_game(const std::filesystem::path& game_root) {
        SettlementLibraryFiles lib;
        lib.where_ = std::string(lzis::kSettlementLibraryPack) + ":";
        for (auto& [name, bytes] : hpfs::read_volume(lzis::read_settlement_library_volume(game_root)))
            lib.files_[lower(name)] = std::move(bytes);
        return lib;
    }
    static SettlementLibraryFiles from_directory(std::filesystem::path dir) {
        SettlementLibraryFiles lib;
        lib.dir_ = std::move(dir);
        lib.where_ = lib.dir_.string() + "/";
        return lib;
    }
    std::vector<std::uint8_t> read(const std::string& name) const {
        const std::string key = "Maps/1/" + name;
        std::vector<std::uint8_t> raw;
        if (!dir_.empty()) {
            const auto p = dir_ / "Maps" / "1" / name;
            std::ifstream in(p, std::ios::binary);
            if (!in) throw std::runtime_error("settlement library file missing: " + p.string());
            raw.assign(std::istreambuf_iterator<char>(in), {});
        } else {
            const auto it = files_.find(lower(key));
            if (it == files_.end()) throw std::runtime_error("settlement library file missing: " + where_ + key);
            raw = it->second;
        }
        for (const auto& [verified, sha] : kVerified)
            if (name == verified && sha256_hex(raw) != sha)
                throw std::runtime_error("settlement library file differs from the verified extraction: " + where_ + key);
        return raw;
    }
    std::string read_text(const std::string& name) const {
        const auto b = read(name);
        return std::string(b.begin(), b.end());
    }

private:
    SettlementLibraryFiles() = default;
    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    static constexpr std::array<std::pair<const char*, const char*>, 3> kVerified{{
        {"map.obj.xml", "b4bb17036687ae3e09023b1c1148954d841d6bf75ada8217ef3f7f503d3a1a18"},
        {"Terrain.decor.grid", "e21eaa0e17b9b3035048537b29815311d546dfab075e6eafb328bb22204cf03b"},
        {"Terrain.terrain.grid", "1b96c3da632a75b690999de9bd89da6a8bf311dc1d0734392eb2eb16da98b246"},
    }};
    std::filesystem::path dir_;
    std::map<std::string, std::vector<std::uint8_t>> files_;
    std::string where_;
};

// "DIRG" grid file: magic, cell, bits, width, height, then cells.
struct GridFile {
    std::uint32_t cell = 0, bits = 0, width = 0, height = 0;
    std::vector<std::uint8_t> payload;
};
inline GridFile parse_grid(const std::vector<std::uint8_t>& raw) {
    if (raw.size() < 20) throw std::runtime_error("truncated grid");
    GridFile g{le32(raw, 4), le32(raw, 8), le32(raw, 12), le32(raw, 16), {}};
    g.payload.assign(raw.begin() + 20, raw.end());
    return g;
}

}  // namespace iberivm::rmdata
