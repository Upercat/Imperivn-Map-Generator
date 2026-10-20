// Settlement library records and the town/village anchor phase (reference: exact._library,
// exact.project, exact.select_template, exact.anchor_phase, exact.settlement_decor_entries,
// exact.library_terrain).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "randommap_data.hpp"
#include "rm_common.hpp"
#include "xml_lite.hpp"

namespace iberivm::rm {

struct LibraryRecord {
    std::string name;
    int cx = 0, cy = 0, w = 0, h = 0;
    std::vector<std::pair<int, int>> comps;  // member offsets from the centre
    std::vector<std::string> classes;
    std::vector<std::vector<std::pair<std::string, std::string>>> members;  // member attributes (regex order)
    std::vector<std::pair<std::string, std::string>> head;                  // settlement attributes
};

struct DecorEntry {
    int dx, dy;
    std::uint16_t v;
};

// Library inputs shared by every stage: records (loader 0x005922A0, claim:
// random-map-settlement-library-record-bbox-center-and-attachment), attached decor entries, the
// library terrain-class grid, plus the game data readers.
struct Library {
    const GameFS* fs = nullptr;
    rmdata::ClassData* classes = nullptr;
    const rmdata::SettlementLibraryFiles* files = nullptr;
    std::vector<LibraryRecord> records;
    std::string obj_text;
    Grid<std::uint16_t> decor;    // library Terrain.decor.grid cells
    Grid<std::uint8_t> terrain;   // library Terrain.terrain.grid cells
    std::vector<std::vector<DecorEntry>> decor_entries;
    static constexpr int neighborhood = 100;  // CONST.INI SettlementsNeighborhood

    rmdata::Rect class_rect(const std::string& cls) const {
        const auto r = classes->class_rect(cls);
        if (!r) throw std::runtime_error("class without entity rect: " + cls);
        return *r;
    }

    void load(const GameFS& gamefs, rmdata::ClassData& cd, const rmdata::SettlementLibraryFiles& lib) {
        fs = &gamefs, classes = &cd, files = &lib;
        obj_text = lib.read_text("map.obj.xml");
        // re.findall(r"<settlement(.*?)>(.*?)</settlement>", text[first:], re.S)
        std::size_t p = obj_text.find("<settlement");
        while (p != std::string::npos) {
            const auto b = obj_text.find("<settlement", p);
            if (b == std::string::npos) break;
            const auto he = obj_text.find('>', b + 11);
            if (he == std::string::npos) break;
            const auto be = obj_text.find("</settlement>", he + 1);
            if (be == std::string::npos) break;
            LibraryRecord rec;
            rec.head = xml::regex_attrs(obj_text.substr(b + 11, he - b - 11));
            const auto* nm = xml::regex_get(rec.head, "name");
            rec.name = nm ? *nm : "";
            for (const auto& body : xml::self_closing_bodies(obj_text, "scriptobj", he + 1, be))
                rec.members.push_back(xml::regex_attrs(body));
            long long L = 0x7D00, T = 0x7D00, R = -1, B = -1;
            for (const auto& m : rec.members) {
                const auto x = rmdata::py_int(*xml::regex_get(m, "x")), y = rmdata::py_int(*xml::regex_get(m, "y"));
                const auto r = class_rect(*xml::regex_get(m, "class"));
                L = std::min(L, x + r.l), T = std::min(T, y + r.t), R = std::max(R, x + r.r), B = std::max(B, y + r.b);
            }
            rec.cx = static_cast<int>(tdiv(L + R, 2)), rec.cy = static_cast<int>(tdiv(T + B, 2));
            rec.w = static_cast<int>(R - L + 1), rec.h = static_cast<int>(B - T + 1);
            for (const auto& m : rec.members) {
                rec.comps.emplace_back(static_cast<int>(rmdata::py_int(*xml::regex_get(m, "x"))) - rec.cx,
                                       static_cast<int>(rmdata::py_int(*xml::regex_get(m, "y"))) - rec.cy);
                rec.classes.push_back(*xml::regex_get(m, "class"));
            }
            records.push_back(std::move(rec));
            p = be + 13;
        }
        const auto dg = rmdata::parse_grid(lib.read("Terrain.decor.grid"));
        decor = Grid<std::uint16_t>(static_cast<int>(dg.height / dg.cell), static_cast<int>(dg.width / dg.cell));
        for (std::size_t i = 0; i < decor.v.size(); ++i)
            decor.v[i] = static_cast<std::uint16_t>(dg.payload.at(2 * i) | (dg.payload.at(2 * i + 1) << 8));
        const auto tg = rmdata::parse_grid(lib.read("Terrain.terrain.grid"));
        terrain = Grid<std::uint8_t>(static_cast<int>(tg.height / tg.cell), static_cast<int>(tg.width / tg.cell));
        for (std::size_t i = 0; i < terrain.v.size(); ++i) terrain.v[i] = tg.payload.at(i);
        // settlement_decor_entries: every library decor cell (rows outer) inside a record rect grown by N
        decor_entries.assign(records.size(), {});
        for (int cy = 0; cy < decor.rows; ++cy)
            for (int cx = 0; cx < decor.cols; ++cx) {
                const auto v = decor.at(cy, cx);
                if (!v) continue;
                const int x = cx * 64, y = cy * 64, hb = v >> 8;
                const int sx = 4 * (hb & 15), sy = 4 * (hb >> 4);
                for (std::size_t i = 0; i < records.size(); ++i) {
                    const auto& r = records[i];
                    const int hw = static_cast<int>(tdiv(r.w, 2)), hh = static_cast<int>(tdiv(r.h, 2));
                    const int n = neighborhood;
                    if (r.cx - hw - n <= x && x <= r.cx + hw + n && r.cy - hh - n <= y && y <= r.cy + hh + n)
                        decor_entries[i].push_back({x + sx - r.cx, y + sy - r.cy, v});
                }
            }
    }
};

// 0x0058E3C0 origin = cx%128 + trunc(x/128)*128, then half-extent clamp
// (claim:random-map-settlement-spawn-projects-origin-and-clamps-half-extent).
inline std::pair<int, int> project(const LibraryRecord& r, int x, int y, int size) {
    long long ox = crem(r.cx, 128) + tdiv(x, 128) * 128;
    long long oy = crem(r.cy, 128) + tdiv(y, 128) * 128;
    const long long hw = tdiv(r.w, 2), hh = tdiv(r.h, 2);
    ox = ox < hw + 1 ? hw + 1 : (ox + hw + 1 >= size ? size - hw - 2 : ox);
    oy = oy < hh + 1 ? hh + 1 : (oy + hh + 1 >= size ? size - hh - 2 : oy);
    return {static_cast<int>(ox), static_cast<int>(oy)};
}

// 0x0058DFC0: prefix hits (max 64); Rnd(0, n-1) at 0x0058E071 when idx is outside [0, n).
inline int select_template(DrawLog& rng, const std::vector<LibraryRecord>& lib, const std::string& prefix, int idx = -1) {
    std::vector<int> hits;
    for (std::size_t i = 0; i < lib.size() && hits.size() < 64; ++i)
        if (lib[i].name.rfind(prefix, 0) == 0) hits.push_back(static_cast<int>(i));
    if (hits.empty()) return -1;
    if (!(0 <= idx && idx < static_cast<int>(hits.size()))) idx = rng(0, static_cast<std::int32_t>(hits.size()) - 1);
    return hits[static_cast<std::size_t>(idx)];
}

struct Slot {
    int k = 0;
    std::pair<int, int> primary{}, village{};
    int town = -1, village_template = -1, village_attempts = 0;
    bool has_village = false;
};

inline const char* const kRaceTownNames[8] = {"Gaul", "Republican Roman", "Carthaginian", "Iberian",
                                               "Imperial Roman", "Briton", "Egyptian", "German"};
constexpr int kBaseVillageDist = 2000, kVillageDistAdd = 50;  // CONST.INI [RandomMap], shipped values

// 0x0057FD60 (claims: random-map-town-village-anchor-phase-draw-sites-and-order,
// random-map-village-candidate-ring-offset-and-acceptance-tests,
// random-map-village-collision-footprints-use-library-record-by-slot-index).
inline std::map<int, Slot> anchor_phase(DrawLog& rng, int size, const std::vector<int>& races, const std::vector<int>& active,
                                        const std::map<int, std::pair<int, int>>& primary, const std::vector<LibraryRecord>& lib) {
    const long long L = 0, T = 0, R = size - 1, B = size - 1;
    const long long m = tdiv(R - L + 1, 16);
    const long long b0 = L + m, b1 = T + m, b2 = R - m, b3 = B - m;
    std::map<int, Slot> slots;
    for (int k : active) {
        slots[k].k = k;
        slots[k].primary = primary.at(k);
    }
    struct Box {
        long long l, t, r, b;
    };
    auto rect = [&](int k, long long x, long long y, long long grow) {
        const long long w = lib.at(static_cast<std::size_t>(k)).w, h = lib.at(static_cast<std::size_t>(k)).h;
        const long long ox = x - tdiv(w, 2), oy = y - tdiv(h, 2);
        return Box{ox - grow, oy - grow, ox + w + grow, oy + h + grow};
    };
    auto hit = [](const Box& a, const Box& b) { return a.l <= b.r && a.r >= b.l && a.t <= b.b && a.b >= b.t; };
    for (int k : active) {
        int race = races[static_cast<std::size_t>(k)];
        if (race == 8) race = (rng(0, 0x1023) >> 4) & 7;
        slots[k].town = select_template(rng, lib, std::string(kRaceTownNames[race]) + " Town");
    }
    const double f001 = f32(0.01);
    for (int k : active) {
        int race = races[static_cast<std::size_t>(k)];
        if (race == 8) race = (rng(0, 0x1023) >> 3) & 7;
        auto& s = slots[k];
        s.village_template = select_template(rng, lib, std::string(kRaceTownNames[race]) + " Village");
        long long c = 0;
        int attempts = 0;
        const auto [bx, by] = s.primary;
        for (;;) {
            const long long r = kBaseVillageDist + c + rng(0, kVillageDistAdd - 1);
            const int a = rng(0, 0x273);
            ++attempts;
            ++c;
            if (c >= (R - m) - (L + m) + 1) c = 0;
            const double theta = a * f001;
            const long long x = bx + static_cast<long long>(std::trunc(static_cast<double>(r) * std::cos(theta)));
            const long long y = by + static_cast<long long>(std::trunc(static_cast<double>(r) * std::sin(theta)));
            if (!(b0 <= x && x <= b2 && b1 <= y && y <= b3)) continue;
            const auto cr = rect(k, x, y, 0);
            if (!(b0 <= cr.l && cr.r <= b2 && b1 <= cr.t && cr.b <= b3)) continue;
            bool blocked = false;
            for (int j : active)
                if (j != k && hit(rect(j, slots[j].primary.first, slots[j].primary.second, 0x100), cr)) blocked = true;
            if (blocked) continue;
            for (int j = 0; j < k; ++j) {
                const auto it = slots.find(j);
                if (it != slots.end() && it->second.has_village &&
                    hit(rect(j, it->second.village.first, it->second.village.second, 0x80), cr))
                    blocked = true;
            }
            if (blocked) continue;
            s.village = {static_cast<int>(x), static_cast<int>(y)};
            s.village_attempts = attempts;
            s.has_village = true;
            break;
        }
    }
    return slots;
}

}  // namespace iberivm::rm
