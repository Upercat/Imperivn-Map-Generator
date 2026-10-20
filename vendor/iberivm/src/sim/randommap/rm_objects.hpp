// Settlement objects, Area templates and the PlaceObjects engine (reference: objects.library_objects,
// objects.settlement_objects, objects.start_respawn_span, static_data.area_templates,
// placeobjects.parse_descriptors and po_engine.Engine, scheduler 0x00589FC0 and its callees).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "randommap_props.hpp"
#include "randommap_rules.hpp"
#include "rm_common.hpp"
#include "rm_fields.hpp"
#include "rm_library.hpp"

namespace iberivm::rm {

using Attrs = std::vector<std::pair<std::string, std::string>>;

inline const std::string* attr(const Attrs& a, const std::string& k) { return xml::regex_get(a, k); }

struct MapObject {
    enum class Kind { member, extra, group, area } kind = Kind::member;
    std::string cls;
    int x = 0, y = 0;
    bool has_facing = false;
    int f1 = 0, f2 = 0;
    int owner = 0;
    int settlement = -1;  // members only
    int partner = -1;     // teleports
    const Attrs* lib = nullptr;
};

struct Settlement {
    bool record = true;
    int idx = -1;
    std::string cls;  // class settlements
    int owner = 0, x = 0, y = 0;
    const Attrs* attrs = nullptr;
    bool adjust = false;
    long long pop_bonus = 0;
};

struct LibraryObjects {
    struct Item {
        std::string cls;
        int dx, dy;
        const Attrs* a;
    };
    struct Rec {
        Attrs head;
        std::vector<Item> members, extras;
    };
    std::vector<Attrs> loose;
    std::vector<Rec> recs;
};

// objects.library_objects: members per record plus the loose library objects attached to the first
// record whose rect grown by the neighbourhood contains them.
inline void load_library_objects(LibraryObjects& out, const Library& lib) {
    const auto& text = lib.obj_text;
    const auto first = text.find("<settlement");
    for (const auto& body : xml::self_closing_bodies(text, "scriptobj", 0, first)) out.loose.push_back(xml::regex_attrs(body));
    out.recs.resize(lib.records.size());
    for (std::size_t i = 0; i < lib.records.size(); ++i) {
        const auto& r = lib.records[i];
        out.recs[i].head = r.head;
        for (std::size_t m = 0; m < r.members.size(); ++m)
            out.recs[i].members.push_back({r.classes[m], r.comps[m].first, r.comps[m].second, &r.members[m]});
    }
    const int n = Library::neighborhood;
    for (const auto& o : out.loose) {
        if (*attr(o, "class") == "AdvArea") continue;
        const auto x = static_cast<int>(rmdata::py_int(*attr(o, "x"))), y = static_cast<int>(rmdata::py_int(*attr(o, "y")));
        for (std::size_t i = 0; i < lib.records.size(); ++i) {
            const auto& r = lib.records[i];
            const int hw = static_cast<int>(tdiv(r.w, 2)), hh = static_cast<int>(tdiv(r.h, 2));
            if (r.cx - hw - n <= x && x <= r.cx + hw + n && r.cy - hh - n <= y && y <= r.cy + hh + n) {
                out.recs[i].extras.push_back({*attr(o, "class"), x - r.cx, y - r.cy, &o});
                break;
            }
        }
    }
}

// CVXItemHolder::Start 0x0053C730 outside the editor: Rnd(0, respawn_time >> 2) at 0x0053C78D when the
// class has a respawn_time other than -15000000 and a respawn_item (objects.start_respawn_span).
inline std::optional<std::int32_t> start_respawn_span(rmdata::ClassProps& props, const std::string& cls) {
    if (props.cpp_class(cls) != std::optional<std::string>("CVXItemHolder")) return std::nullopt;
    const auto t = props.prop(cls, "respawn_time");
    const auto item = props.prop(cls, "respawn_item");
    if (!t || !item || *item == "**Invalid**" || rmdata::py_int(*t) == -15000000) return std::nullopt;
    return static_cast<std::int32_t>((static_cast<std::uint32_t>(rmdata::py_int(*t))) >> 2);
}

// 0x0058E3C0 with a library record (objects.settlement_objects).
inline void settlement_objects(std::vector<MapObject>& objects, std::vector<Settlement>& settlements, const Library& lib,
                               const LibraryObjects& lo, int idx, std::pair<int, int> anchor, int size, int owner, int sid,
                               DrawLog* start_rng, rmdata::ClassProps& props) {
    const auto [xo, yo] = project(lib.records.at(static_cast<std::size_t>(idx)), anchor.first, anchor.second, size);
    const auto& rec = lo.recs.at(static_cast<std::size_t>(idx));
    Settlement s;
    s.record = true, s.idx = idx, s.owner = owner, s.x = xo, s.y = yo, s.attrs = &rec.head;
    settlements.push_back(s);
    auto start = [&](const std::string& cls) {
        if (!start_rng) return;
        if (const auto span = start_respawn_span(props, cls)) (*start_rng)(0, *span);
    };
    for (const auto& m : rec.members) {
        MapObject o;
        o.kind = MapObject::Kind::member, o.cls = m.cls, o.x = xo + m.dx, o.y = yo + m.dy, o.settlement = sid, o.owner = owner,
        o.lib = m.a;
        objects.push_back(o);
        start(m.cls);
    }
    for (const auto& e : rec.extras) {
        const int x = xo + e.dx, y = yo + e.dy;
        if (0 <= x && x < size && 0 <= y && y < size) {
            MapObject o;
            o.kind = MapObject::Kind::extra, o.cls = e.cls, o.x = x, o.y = y, o.owner = owner, o.lib = e.a;
            objects.push_back(o);
            start(e.cls);
        }
    }
}

struct AreaTemplate {
    std::string name;
    int num = 0;
    int rect[4] = {0, 0, 0, 0};
    struct Obj {
        std::string name;
        int dx, dy;
        long long r2e0;
    };
    std::vector<Obj> objects;
    std::vector<DecorEntry> decor;
    struct Cell {
        int dx, dy, v;
    };
    std::vector<Cell> terrain;
};

// static_data.area_templates: AdvArea objects of the library map, named by the <group> elements.
inline std::vector<AreaTemplate> load_area_templates(const Library& lib, const LibraryObjects& lo, rmdata::ClassProps& props) {
    const auto& text = lib.obj_text;
    std::map<std::string, std::string> names;
    // re.findall(r'<group\s+name="([^"]*)"[^>]*>(.*?)</group>', text, re.S)
    for (std::size_t p = 0;;) {
        const auto b = text.find("<group", p);
        if (b == std::string::npos) break;
        p = b + 1;
        std::size_t q = b + 6;
        auto ws = [&](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
        if (q >= text.size() || !ws(text[q])) continue;
        while (q < text.size() && ws(text[q])) ++q;
        if (text.compare(q, 6, "name=\"") != 0) continue;
        const auto ne = text.find('"', q + 6);
        if (ne == std::string::npos) continue;
        const auto name = text.substr(q + 6, ne - q - 6);
        const auto gt = text.find('>', ne + 1);
        if (gt == std::string::npos) continue;
        const auto end = text.find("</group>", gt + 1);
        if (end == std::string::npos) continue;
        const auto body = text.substr(gt + 1, end - gt - 1);
        for (std::size_t s = 0;;) {
            const auto nb = body.find("num=\"", s);
            if (nb == std::string::npos) break;
            s = nb + 1;
            std::size_t d = nb + 5;
            while (d < body.size() && body[d] >= '0' && body[d] <= '9') ++d;
            if (d == nb + 5 || d >= body.size() || body[d] != '"') continue;
            names[body.substr(nb + 5, d - nb - 5)] = name;
        }
        p = end + 8;
    }
    std::vector<AreaTemplate> areas;
    for (const auto& o : lo.loose) {
        if (*attr(o, "class") != "AdvArea") continue;
        const auto L = rmdata::py_int(*attr(o, "left")), T = rmdata::py_int(*attr(o, "top"));
        const auto R = rmdata::py_int(*attr(o, "right")), B = rmdata::py_int(*attr(o, "bottom"));
        AreaTemplate ar;
        const auto num = *attr(o, "num");
        ar.name = names.count(num) ? names[num] : "";
        ar.num = static_cast<int>(rmdata::py_int(num));
        ar.rect[0] = static_cast<int>(tdiv(L, 64) * 64), ar.rect[1] = static_cast<int>(tdiv(T, 64) * 64);
        ar.rect[2] = static_cast<int>(tdiv(R + 63, 64) * 64 - 1), ar.rect[3] = static_cast<int>(tdiv(B + 63, 64) * 64 - 1);
        areas.push_back(ar);
    }
    const int n = Library::neighborhood;
    auto in_record = [&](int x, int y) {
        for (const auto& r : lib.records) {
            const int hw = static_cast<int>(tdiv(r.w, 2)), hh = static_cast<int>(tdiv(r.h, 2));
            if (r.cx - hw - n <= x && x <= r.cx + hw + n && r.cy - hh - n <= y && y <= r.cy + hh + n) return true;
        }
        return false;
    };
    auto area_of = [&](int x, int y) -> AreaTemplate* {
        for (auto& ar : areas)
            if (ar.rect[0] <= x && x <= ar.rect[2] && ar.rect[1] <= y && y <= ar.rect[3]) return &ar;
        return nullptr;
    };
    for (const auto& o : lo.loose) {
        if (*attr(o, "class") == "AdvArea") continue;
        const int x = static_cast<int>(rmdata::py_int(*attr(o, "x"))), y = static_cast<int>(rmdata::py_int(*attr(o, "y")));
        if (in_record(x, y)) continue;
        if (auto* ar = area_of(x, y)) {
            const auto sr = props.prop(*attr(o, "class"), "selection_radius");
            ar->objects.push_back({*attr(o, "class"), x - ar->rect[0], y - ar->rect[1], sr ? rmdata::py_int(*sr) : -15000000});
        }
    }
    for (int cy = 0; cy < lib.decor.rows; ++cy)
        for (int cx = 0; cx < lib.decor.cols; ++cx) {
            const auto v = lib.decor.at(cy, cx);
            if (!v) continue;
            const int x = cx * 64, y = cy * 64;
            if (auto* ar = area_of(x, y)) {
                const int hb = v >> 8;
                ar->decor.push_back({x + 4 * (hb & 15) - ar->rect[0], y + 4 * (hb >> 4) - ar->rect[1], v});
            }
        }
    for (auto& ar : areas) {
        const int L = ar.rect[0], T = ar.rect[1], R = ar.rect[2], B = ar.rect[3];
        const int w64 = static_cast<int>(tdiv(R - L + 0x21, 64) * 64), h64 = static_cast<int>(tdiv(B - T + 0x21, 64) * 64);
        for (int dy = 0; dy < h64; dy += 64)
            for (int dx = 0; dx < w64; dx += 64)
                ar.terrain.push_back({dx, dy, lib.terrain.at(py_index((T + dy) >> 6, lib.terrain.rows), py_index((L + dx) >> 6, lib.terrain.cols))});
    }
    return areas;
}

// --- PlaceObjects descriptors (placeobjects.parse_descriptors) -------------------------------------

struct Descriptor {
    std::string setname, cls;
    long long min = 0, max = 0, mingroup = 1, maxgroup = 1, infmax = 10, inf = 20, fade = 1, benefit = 0, mindist = 1500,
              rescue = 0, groupdist = 64, water = 0;
    std::vector<std::pair<int, int>> points;
    std::vector<std::pair<int, std::string>> alts;
    long long attempts = 0, successes = 0;
};

inline const char* const kNoFlags[12] = {"decorations", "itemholders", "outposts", "ruins",      "shipyards", "stonehenge",
                                         "strongholds", "teleports",   "teutontents", "trees", "villages",  "wildlife"};

// 0x00587720: symbol X defined when the rms dword no_X (0x38 + 4i) is zero.
inline std::vector<std::string> symbols(const std::map<int, int>& flags) {
    std::vector<std::string> out;
    for (int i = 0; i < 12; ++i) {
        const auto it = flags.find(0x38 + 4 * i);
        if (it == flags.end() || it->second == 0) out.push_back(kNoFlags[i]);
    }
    return out;
}

inline long long py_atoi(const std::string* s) {
    if (!s) return 0;
    std::size_t p = 0;
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    while (p < s->size() && ws((*s)[p])) ++p;
    const auto b = p;
    if (p < s->size() && ((*s)[p] == '+' || (*s)[p] == '-')) ++p;
    const auto d = p;
    while (p < s->size() && (*s)[p] >= '0' && (*s)[p] <= '9') ++p;
    if (p == d) return 0;
    return std::stoll(s->substr(b, p - b));
}

inline std::vector<Descriptor> parse_descriptors(const std::string& text, int size, int players, const std::vector<std::string>& defined,
                                                 rmdata::ClassProps& props) {
    auto is_defined = [&](const std::string* id) {
        return id && std::find(defined.begin(), defined.end(), *id) != defined.end();
    };
    auto water_class = [&](const std::string& cls) -> long long {
        const auto v = cls.empty() ? std::optional<std::string>("0") : props.prop(cls, "water_unit");
        const std::string s = v ? *v : "0";
        return (s.empty() ? 0 : rmdata::py_int(s)) != 0 ? 1 : 0;
    };
    auto word = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
               (c >= 0xC0 && c != 0xD7 && c != 0xF7) || c == 0xAA || c == 0xB5 || c == 0xBA;
    };
    auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v'; };
    static const std::pair<const char*, int> race_ids[] = {{"Gaul", 0},         {"RepublicanRome", 1}, {"Carthage", 2}, {"Iberia", 3},
                                                           {"ImperialRome", 4}, {"Britain", 5},        {"Egypt", 6},    {"Germany", 7}};
    std::vector<Descriptor> descs;
    bool active = false, skip = false;
    std::size_t pos = 0;
    while (true) {
        const auto lt = text.find('<', pos);
        if (lt == std::string::npos) break;
        // <(/?)([A-Za-z_][\w-]*)((?:\s+[\w-]+\s*=\s*"[^"]*")*)\s*(/?)>
        std::size_t p = lt + 1;
        bool closing = false;
        if (p < text.size() && text[p] == '/') closing = true, ++p;
        if (p >= text.size() || !((text[p] >= 'A' && text[p] <= 'Z') || (text[p] >= 'a' && text[p] <= 'z') || text[p] == '_')) {
            pos = lt + 1;
            continue;
        }
        const auto tb = p;
        while (p < text.size() && (word(static_cast<unsigned char>(text[p])) || text[p] == '-')) ++p;
        const auto tag = text.substr(tb, p - tb);
        Attrs a;
        for (;;) {  // greedy attribute group with backtracking to the last complete attribute
            auto q = p;
            if (q >= text.size() || !ws(text[q])) break;
            while (q < text.size() && ws(text[q])) ++q;
            const auto kb = q;
            while (q < text.size() && (word(static_cast<unsigned char>(text[q])) || text[q] == '-')) ++q;
            if (q == kb) break;
            const auto key = text.substr(kb, q - kb);
            while (q < text.size() && ws(text[q])) ++q;
            if (q >= text.size() || text[q] != '=') break;
            ++q;
            while (q < text.size() && ws(text[q])) ++q;
            if (q >= text.size() || text[q] != '"') break;
            const auto ve = text.find('"', q + 1);
            if (ve == std::string::npos) break;
            bool replaced = false;
            for (auto& kv : a)
                if (kv.first == key) kv.second = text.substr(q + 1, ve - q - 1), replaced = true;
            if (!replaced) a.emplace_back(key, text.substr(q + 1, ve - q - 1));
            p = ve + 1;
        }
        auto q = p;
        while (q < text.size() && ws(text[q])) ++q;
        if (q < text.size() && text[q] == '/') ++q;
        if (q >= text.size() || text[q] != '>') {
            pos = lt + 1;
            continue;
        }
        pos = q + 1;
        const auto tl = rmdata::lower(tag);
        if (closing) {
            if (tl == "ifdef" || tl == "ifndef") skip = false;
            else if (tl == "placegameobjects" && active && !descs.empty())
                return descs;  // 0x0057B94F: the end tag runs the scheduler on this list
            continue;
        }
        if (skip) continue;
        if (tl == "ifdef") skip = !is_defined(attr(a, "id"));
        else if (tl == "ifndef") skip = is_defined(attr(a, "id"));
        else if (tl == "placegameobjects") {
            descs.clear();
            const auto* msv = attr(a, "mapsize");
            const std::string zero = "0";
            const auto ms = py_atoi(msv ? msv : &zero);
            active = ms == 0 || ms == size;
        } else if (tl == "gameobj" && active && descs.size() < 0x100) {
            Descriptor d;
            if (const auto* v = attr(a, "SetName")) d.setname = *v;
            if (const auto* v = attr(a, "class")) d.cls = *v;
            long long mn = attr(a, "min") ? py_atoi(attr(a, "min")) : -1;
            long long mx = attr(a, "max") ? py_atoi(attr(a, "max")) : -1;
            if (mn < 0 && mx < 0) {
                mn = std::max(players * py_atoi(attr(a, "minperplayer")), 0ll);
                const long long pmx = players * py_atoi(attr(a, "maxperplayer"));
                mx = pmx > mn ? pmx : mn;
            } else {
                mn = std::max(mn, 0ll);
                mx = std::max(mx, mn);
            }
            d.min = mn, d.max = mx;
            const std::pair<const char*, long long Descriptor::*> fields[] = {
                {"inf-max", &Descriptor::infmax}, {"inf", &Descriptor::inf},           {"fade", &Descriptor::fade},
                {"benefit", &Descriptor::benefit}, {"mindist", &Descriptor::mindist},   {"mingroup", &Descriptor::mingroup},
                {"maxgroup", &Descriptor::maxgroup}, {"groupdist", &Descriptor::groupdist}};
            for (const auto& [key, field] : fields)
                if (const auto* v = attr(a, key)) d.*field = py_atoi(v);
            if (const auto* v = attr(a, "rescue")) d.rescue = py_atoi(v) != 0 ? 1 : 0;
            if (const auto* v = attr(a, "water")) {
                const auto w = rmdata::lower(*v);
                d.water = (w == "1" || w == "yes" || w == "true" || w == "water") ? 1 : 0;
            } else
                d.water = water_class(d.cls);
            descs.push_back(d);
        } else if (tl == "racealternative" && active && !descs.empty()) {
            int rid = -1;
            const auto* race = attr(a, "race");
            for (const auto& [nm, id] : race_ids)
                if (race && *race == nm) rid = id;
            const auto* c = attr(a, "class");
            descs.back().alts.emplace_back(rid, c ? *c : "");
        }
    }
    return {};
}

// --- PlaceObjects engine (po_engine.Engine) --------------------------------------------------------

inline const char* const kVillagePrefix[8] = {"Gaul ",     "Republican Roman ", "Imperial Roman ", "Carthaginian ",
                                              "Iberian ",  "Briton ",           "Egyptian ",       "German "};  // 0x0058988D
constexpr int kTeleportMinDist = 33;  // CONST.INI [RandomMap] TeleportMinDist

// 0x0058D9A0: diamond rings 0xFF / 0xC0 / 0x80 / 0x40 OR-combined into the 512x512 byte mask.
inline void stamp_diamond(Grid<std::uint8_t>& mask, long long cx, long long cy, long long r) {
    for (long long i = 0; i <= r; ++i)
        for (long long j = 0; j <= r - i; ++j) {
            const long long dd = i + j;
            const std::uint8_t v = dd < r - 4 ? 0xFF : (dd < r - 2 ? 0xC0 : (dd < r ? 0x80 : 0x40));
            const long long pos[4][2] = {{cy + i, cx + j}, {cy + i, cx - j}, {cy - i, cx + j}, {cy - i, cx - j}};
            for (const auto& p : pos)
                if (0 <= p[0] && p[0] < 512 && 0 <= p[1] && p[1] < 512) mask.at(static_cast<int>(p[0]), static_cast<int>(p[1])) |= v;
        }
}

inline long long isqrt_masked(long long v) { return isqrt(v & 0xFFFFFFFFll); }

class PlaceObjects {
public:
    PlaceObjects(DrawLog& rng, int size, std::vector<Descriptor> descs, const std::map<int, Slot>& slots, const std::vector<int>& races,
                 const Library& lib, const LibraryObjects& lo, const std::vector<AreaTemplate>& areas, std::vector<std::uint32_t> packed,
                 Grid<std::uint8_t> zero, Grid<std::uint16_t>& decor, Grid<float>& a, Grid<std::uint8_t>& cls, rmdata::ClassProps& props,
                 const rmdata::DecorRegistry& decor_reg, std::vector<Settlement>& settlement_base, bool game_start)
        : rng_(rng), size_(size), d_(std::move(descs)), slots_(slots), races_(races), lib_(lib), lo_(lo), areas_(areas),
          g_(std::move(packed)), W_(static_cast<int>(tdiv(size, 128))), H_(W_), zero_(std::move(zero)), decor_(decor), a_(a), cls_(cls),
          props_(props), decor_reg_(decor_reg), base_(settlement_base), game_start_(game_start) {
        vec_.assign(8, -1.0f);
        for (const auto& [k, s] : slots_) vec_[static_cast<std::size_t>(k)] = 0.0f;
        for (const auto& [k, s] : slots_) by_race_[races_[static_cast<std::size_t>(k)]].push_back(s.primary);
    }

    std::vector<MapObject> objects;
    std::vector<Settlement> settlements;

    // 0x00589FC0: phase 1 (min quotas, flag Rnd(0,3)), phase 2 (max quotas, flag Rnd(0,1)).
    void run() {
        for (auto& d : d_) {
            d.attempts = d.successes = 0;
            if (d.setname == "Town")
                for (const auto& [k, s] : slots_) d.points.push_back(s.primary);
            else if (d.setname == "Village")
                for (const auto& [k, s] : slots_) d.points.push_back(s.village);
        }
        long long smin = 0, smax = 0;
        for (const auto& d : d_) smin += d.min, smax += d.max;
        for (int phase = 0; phase < 2; ++phase) {
            const long long S = phase == 0 ? smin : smax;
            const long long k0 = phase == 0 ? 0 : smin, k1 = phase == 0 ? smin : smax;
            const int fmax = phase == 0 ? 3 : 1;
            for (long long k = k0; k < k1; ++k) {
                long long best = 0x100000;
                int bi = -1;
                for (std::size_t i = 0; i < d_.size(); ++i) {
                    const auto& d = d_[i];
                    const long long q = phase == 0 ? d.min : d.max;
                    if (d.attempts >= q) continue;
                    const long long v = std::llabs(tdiv((d.attempts + 1) << 10, q) - tdiv(k << 10, S));
                    if (v < best) best = v, bi = static_cast<int>(i);
                }
                const int flag = rng_(0, fmax) == 0 ? 1 : 0;
                auto& d = d_.at(static_cast<std::size_t>(bi < 0 ? static_cast<int>(d_.size()) + bi : bi));
                const int r = attempt(d, flag);
                if (r) ++d.successes;
                ++d.attempts;
            }
        }
    }

private:
    DrawLog& rng_;
    int size_;
    std::vector<Descriptor> d_;
    const std::map<int, Slot>& slots_;
    const std::vector<int>& races_;
    const Library& lib_;
    const LibraryObjects& lo_;
    const std::vector<AreaTemplate>& areas_;
    std::vector<std::uint32_t> g_;
    int W_, H_;
    Grid<std::uint8_t> zero_;
    Grid<std::uint16_t>& decor_;
    Grid<float>& a_;
    Grid<std::uint8_t>& cls_;
    rmdata::ClassProps& props_;
    const rmdata::DecorRegistry& decor_reg_;
    std::vector<Settlement>& base_;
    bool game_start_;
    std::vector<float> vec_;
    std::map<int, std::vector<std::pair<int, int>>> by_race_;
    int rescue_ = 0;
    std::optional<std::size_t> last_obj_;
    std::map<std::string, std::optional<rmdata::Rect>> decor_rect_cache_;

    std::size_t settlement_count() const { return base_.size() + settlements.size(); }
    Settlement& settlement_at(std::size_t i) { return i < base_.size() ? base_[i] : settlements[i - base_.size()]; }

    int inf_val_at(long long cx, long long cy) const { return inf_val(g_[static_cast<std::size_t>(py_index(cy * W_ + cx, static_cast<int>(g_.size())))]); }

    // 0x0057B9F0 (claim:random-map-influence-stamp-diamond-cone-with-ring-early-stop).
    void stamp(long long x, long long y, long long inf, long long fade) {
        const long long cx = tdiv(x, 128), cy = tdiv(y, 128);
        long long r = 0, w = inf;
        while (r <= inf) {
            bool wrote = false;
            for (long long k = 0; k <= r; ++k) {
                const long long pts[4][2] = {{cx - k, cy - (r - k)}, {cx + k, cy - (r - k)}, {cx - k, cy + (r - k)}, {cx + k, cy + (r - k)}};
                for (const auto& p : pts)
                    if (0 <= p[0] && p[0] < W_ && 0 <= p[1] && p[1] < H_) {
                        auto& c = g_[static_cast<std::size_t>(p[1] * W_ + p[0])];
                        if (inf_val(c) < w) {
                            c = inf_put(c, static_cast<int>(w), static_cast<int>(fade));
                            wrote = true;
                        }
                    }
            }
            if (!wrote) break;
            w -= fade;
            ++r;
        }
    }

    // 0x0057BCB0 (claim:random-map-influence-descent-steps-to-strictly-lower-neighbour).
    std::optional<std::pair<int, int>> descend(int x, int y, long long thr) const {
        long long cx = tdiv(x, 128), cy = tdiv(y, 128);
        int v = inf_val_at(cx, cy);
        if (v <= thr) return std::make_pair(x, y);
        for (;;) {
            int best = v;
            long long bx = -1, by = -1;
            bool any = false;
            const long long nb[4][3] = {{cx - 1, cy, cx > 0}, {cx, cy - 1, cy > 0}, {cx + 1, cy, cx + 1 < W_}, {cx, cy + 1, cy + 1 < H_}};
            for (const auto& e : nb)
                if (e[2]) {
                    const int nv = inf_val_at(e[0], e[1]);
                    if (nv < best) best = nv, bx = e[0], by = e[1], any = true;
                }
            if (!any) return std::nullopt;
            cx = bx, cy = by, v = best;
            if (v <= thr) return std::make_pair(static_cast<int>(cx * 128), static_cast<int>(cy * 128));
        }
    }

    int water(long long x, long long y) const { return zero_.at(py_index(y >> 5, zero_.rows), py_index(x >> 5, zero_.cols)); }

    std::pair<long long, long long> margins(const Descriptor& d) {
        if (!d.setname.empty()) {
            long long mx = 0, my = 0;
            for (const auto& r : lib_.records)
                if (r.name.find(d.setname) != std::string::npos) {
                    mx = std::max(mx, tdiv(r.w, 32) + 2);
                    my = std::max(my, tdiv(r.h, 32) + 2);
                }
            return {mx, my};
        }
        std::optional<long long> f;
        if (!d.cls.empty())
            if (const auto v = props_.prop(d.cls, "radius")) f = rmdata::py_int(*v);
        if (!f) return {10, 10};
        const long long mx = tdiv(*f + 0x20, 32) + 2;
        return {mx, mx + 4};
    }

    // 0x00588C10 (P12 claims 15-19).
    std::optional<std::pair<int, int>> candidate(const Descriptor& d) {
        const auto [mx, my] = margins(d);
        const long long cw = (size_ >> 5) - 2 * mx, ch = (size_ >> 5) - 2 * my;
        for (int t = 0; t < 100; ++t) {
            const long long x = (mx + rng_(0, static_cast<std::int32_t>(cw - 1))) * 32;
            const long long y = (my + rng_(0, static_cast<std::int32_t>(ch - 1))) * 32;
            std::pair<int, int> pt;
            if (d.water) {
                if (!water(x, y)) continue;
                pt = {static_cast<int>(x), static_cast<int>(y)};
            } else {
                const auto p = descend(static_cast<int>(x), static_cast<int>(y), d.infmax & 0xFFFF);
                if (!p) continue;
                pt = *p;
            }
            if (d.mindist) {
                bool too_close = false;
                for (const auto& [px, py] : d.points) {
                    const long long dx = pt.first - px, dy = pt.second - py;
                    if (isqrt(dx * dx + dy * dy) < d.mindist) {
                        too_close = true;
                        break;
                    }
                }
                if (too_close) continue;
            }
            return pt;
        }
        return std::nullopt;
    }

    // 0x005883D0 (claim:random-map-benefit-contribution-per-player-slot-is-map-width-over-anchor-distance).
    std::vector<float> add_benefit(const std::vector<float>& vec, std::pair<int, int> pt, long long b) const {
        std::vector<double> contrib(8, -1.0);
        for (const auto& [k, s] : slots_) {
            const double dx = pt.first - s.primary.first, dy = pt.second - s.primary.second;
            contrib[static_cast<std::size_t>(k)] = static_cast<double>(f32(size_ / std::sqrt(dx * dx + dy * dy)));
        }
        auto out = vec;
        for (std::size_t j = 0; j < 8; ++j)
            if (static_cast<double>(out[j]) >= 0.0) out[j] = f32(static_cast<double>(out[j]) + contrib[j] * static_cast<double>(b));
        return out;
    }

    // 0x005881E0 coefficient of variation over entries >= 0, in the native order (0x005881E5..0x005883BF, x87 at
    // 53-bit precision): sequential sums and d*d (research/randommap-numerics.md S2).
    static double cv(const std::vector<float>& vec) {
        std::vector<double> vals;
        for (float v : vec)
            if (static_cast<double>(v) >= 0.0) vals.push_back(v);
        const auto n = vals.size();
        double s = 0.0;
        for (double v : vals) s += v;
        const double mean = s * (1.0 / static_cast<double>(n));
        double acc = 0.0;
        for (double v : vals) {
            const double d = v - mean;
            acc += d * d;
        }
        return static_cast<double>(f32(std::sqrt(acc * static_cast<double>(f32(1.0 / static_cast<double>(n)))) / mean));
    }

    // 0x0057DE00 (claim:random-map-nearest-stronghold-race-by-point-lists).
    int nearest_race(std::pair<int, int> pt) const {
        long long best = 0xFFFFFFFFll;
        int br = 0;
        for (int r = 0; r < 8; ++r) {
            const auto it = by_race_.find(r);
            if (it == by_race_.end()) continue;
            for (const auto& [x, y] : it->second) {
                const long long dx = pt.first - x, dy = pt.second - y;
                const long long dd = isqrt_masked(dx * dx + dy * dy);
                if (dd < best) best = dd, br = r;
            }
        }
        return br;
    }

    // 0x005887F0: two Rnd(0, 30) per created object of a qualifying class.
    std::pair<int, int> facing() {
        const int r1 = rng_(0, 30);
        const int r2 = rng_(0, 30);
        return {r1, r2};
    }

    static std::string race_class(const Descriptor& d, int race) {
        std::string cls = d.cls;
        for (const auto& [rid, c] : d.alts)
            if (rid == race) cls = c;
        return cls;
    }

    void add_group_object(const std::string& cls, std::pair<int, int> p) {
        MapObject o;
        o.kind = MapObject::Kind::group, o.cls = cls, o.x = p.first, o.y = p.second, o.owner = 14;
        const auto f = facing();
        o.has_facing = true, o.f1 = f.first, o.f2 = f.second;
        objects.push_back(o);
    }

    // 0x00589410 (P12 claims 29-36).
    int group(Descriptor& d, std::pair<int, int> pt, int race, const std::string& cls_in) {
        if (water(pt.first, pt.second) != d.water) return 0;
        long long target = d.mingroup + rng_(0, static_cast<std::int32_t>(d.maxgroup - d.mingroup - 1));
        const bool beast = d.cls == "Wolf" || d.cls == "Bear" || d.cls == "Boar";
        if (beast && (race == 2 || race == 6)) {
            d.mingroup = d.maxgroup = 2;
            target = 2;
        }
        const std::string cls = cls_in.empty() ? d.cls : cls_in;
        add_group_object(cls, pt);
        std::vector<std::pair<int, int>> members{pt};
        const int lw = water(pt.first, pt.second);
        if (target == 1) return 1;
        const long long gd = d.groupdist;
        for (long long t = 0; t < target * 100; ++t) {
            const int a = rng_(0, 627);
            const long long dx = static_cast<long long>(std::trunc(std::cos(a * 0.01) * gd * 1.2));
            const long long dy = static_cast<long long>(std::trunc(std::sin(a * 0.01) * gd * 1.2));
            const int anc = rng_(0, static_cast<std::int32_t>(members.size()) - 1);
            const std::pair<int, int> p{static_cast<int>(members[static_cast<std::size_t>(anc)].first + dx),
                                        static_cast<int>(members[static_cast<std::size_t>(anc)].second + dy)};
            if (!(0 <= p.first && p.first <= size_ - 1 && 0 <= p.second && p.second <= size_ - 1)) continue;
            if (water(p.first, p.second) != lw) continue;
            bool close = false;
            for (const auto& m : members) {
                const long long ex = p.first - m.first, ey = p.second - m.second;
                if (isqrt(ex * ex + ey * ey) < gd) {
                    close = true;
                    break;
                }
            }
            if (close) continue;
            // 0x00589701: each extra member takes the class for the nearest stronghold race at its own point; a Wolf,
            // Bear or Boar member in Carthage (2) or Egypt (6) becomes LionF (0x0058978F)
            const int mrace = nearest_race(p);
            const std::string mcls = beast && (mrace == 2 || mrace == 6) ? std::string("LionF") : race_class(d, mrace);
            add_group_object(mcls, p);
            members.push_back(p);
            if (static_cast<long long>(members.size()) == target) break;
        }
        d.points.push_back(pt);
        return 1;
    }

    // Mask blend shared by 0x0058E3C0 and 0x0058F570: A = ((255-m)*A + m*mean) * f32(1/255).
    void blend(const Grid<std::uint8_t>& mask, float mean, long long xs, long long xe, long long ys, long long ye, long long ox, long long oy) {
        const double inv = f32(1.0 / 255.0);
        const int n = a_.rows;
        for (long long row = ys; row < ye; ++row)
            for (long long col = xs; col < xe; ++col) {
                const int m = mask.at(static_cast<int>(row), static_cast<int>(col));
                if (!m) continue;
                const int i = py_index((oy + row) * n + col + ox, n * n);
                auto& cell = a_.v[static_cast<std::size_t>(i)];
                cell = f32(((255 - m) * static_cast<double>(cell) + m * static_cast<double>(mean)) * inv);
            }
    }

    rmdata::Rect class_rect(const std::string& cls) const { return lib_.class_rect(cls); }

    std::optional<rmdata::Rect> decor_rect(int t) {
        const auto key = std::to_string(t);
        if (const auto it = decor_rect_cache_.find(key); it != decor_rect_cache_.end()) return it->second;
        std::optional<rmdata::Rect> r;
        const auto e = decor_reg_.entity.find(t);
        if (e != decor_reg_.entity.end()) r = lib_.classes->entity_rect(e->second);
        decor_rect_cache_[key] = r;
        return r;
    }

    // 0x0058E3C0 without a library record: one component at pt, flattened with the entity rect +0x288, decor
    // disc cleared around the rect bbox (claim:random-map-settlement-spawn-flattens-height-to-one-floored-mean).
    void spawn_class(const std::string& cls, std::pair<int, int> pt) {
        const auto rc = class_rect(cls);
        const long long L = rc.l, T = rc.t, R = rc.r, B = rc.b;
        const long long x = pt.first, y = pt.second;
        const int n = a_.rows;
        const int sid = static_cast<int>(settlement_count());
        Settlement s;
        s.record = false, s.cls = cls, s.owner = 14 + rescue_, s.x = pt.first, s.y = pt.second;
        settlements.push_back(s);
        MapObject o;
        o.kind = MapObject::Kind::member, o.cls = cls, o.x = pt.first, o.y = pt.second, o.settlement = sid, o.owner = 14 + rescue_;
        objects.push_back(o);
        last_obj_ = objects.size() - 1;
        Grid<std::uint8_t> mask(512, 512, 0);
        const long long rl = tdiv(L, 32) - 8, rt = tdiv(T, 32) - 8, rr = tdiv(R, 32) + 8, rb = tdiv(B, 32) + 8;
        stamp_diamond(mask, tdiv(rl + rr, 2) + 255, tdiv(rt + rb, 2) + 255,
                      static_cast<long long>(std::trunc(static_cast<double>(tdiv(std::max(rr - rl + 1, rb - rt + 1), 2)) *
                                                        static_cast<double>(f32(1.42)))));
        const long long ox = tdiv(x, 32), oy = tdiv(y, 32);
        auto A = [&](long long r_, long long c_) { return a_.at(py_index(r_, n), py_index(c_, n)); };
        double acc = 0.0;
        for (float v : {A(oy, ox), A(oy, std::max(rl + ox, 0ll)), A(std::max(rt + oy, 0ll), ox), A(oy, std::min(rr + ox, static_cast<long long>(n - 1))),
                        A(std::min(rb + oy, static_cast<long long>(n - 1)), ox)})
            acc += static_cast<double>(v);  // x87 accumulation, one FSTP
        const float total = f32(acc);
        float mean = f32(static_cast<double>(total) / 5);
        if (static_cast<double>(mean) < static_cast<double>(f32(0.1))) mean = f32(0.1);
        const long long bl = std::min(x, x + L), br = std::max(x, x + R), bt = std::min(y, y + T), bb = std::max(y, y + B);
        const long long u = isqrt((bt - bb) * (bt - bb) + (bl - br) * (bl - br));
        clear_decor_disc(decor_, static_cast<int>(tdiv(bl + br, 2)), static_cast<int>(tdiv(bt + bb, 2)), tdiv(u >> 1, 5) + (u >> 1), size_);
        const long long xs = std::max(255 - ox, 0ll), xe = std::min(tdiv(size_, 32) - ox + 255, 512ll);
        const long long ys = std::max(255 - oy, 0ll), ye = std::min(tdiv(size_, 32) - oy + 255, 512ll);
        blend(mask, mean, xs, xe, ys, ye, ox - 255, oy - 255);
    }

    // 0x0058F570 (claims random-map-project-area-template, random-map-area-template-decor-clear-centred-on-projected-corner,
    // random-map-area-template-clears-then-copies-decor).
    bool project_area(int idx, std::pair<int, int> pt) {
        const auto& ar = areas_.at(static_cast<std::size_t>(idx));
        const long long L = ar.rect[0], T = ar.rect[1], R = ar.rect[2], B = ar.rect[3];
        const long long size = size_;
        const long long xp = tdiv(pt.first - tdiv(R - L + 1, 2) + 0x20, 64) * 64;
        const long long yp = tdiv(pt.second - tdiv(B - T + 1, 2) + 0x20, 64) * 64;
        if (xp < 0 || yp < 0) return false;
        const long long w64 = tdiv(R - L + 0x21, 64) * 64, h64 = tdiv(B - T + 0x21, 64) * 64;
        if (xp + w64 >= size || yp + h64 >= size) return false;
        for (const auto& c : ar.terrain)
            if (c.v != 0xF) cls_.at(py_index((yp + c.dy) >> 6, cls_.rows), py_index((xp + c.dx) >> 6, cls_.cols)) = static_cast<std::uint8_t>(c.v);
        const long long u = isqrt(h64 * h64 + w64 * w64);
        clear_decor_disc(decor_, static_cast<int>(xp), static_cast<int>(yp), tdiv(u >> 1, 5) + (u >> 1), size_);
        Grid<std::uint8_t> mask(512, 512, 0);
        const long long mx0 = tdiv(512 - (w64 >> 5), 2), my0 = tdiv(512 - (h64 >> 5), 2);
        const int n = a_.rows;
        float total = 0.0f;
        int count = 0;
        auto sample = [&](long long l, long long t, long long r_, long long b) {
            double acc = total;
            const long long pts[4][2] = {{t, l}, {t, r_}, {b, l}, {b, r_}};
            for (const auto& p : pts) acc += static_cast<double>(a_.at(py_index(tdiv(p[0], 32), n), py_index(tdiv(p[1], 32), n)));
            total = f32(acc);
            count += 4;
        };
        for (const auto& o : ar.objects) {
            long long r = o.r2e0;
            if (r < 1 || r == -15000000) r = 30;
            const long long ex = xp + o.dx, ey = yp + o.dy;
            if (!(0 <= ex - r && ex + r <= size - 1 && 0 <= ey - r && ey + r <= size - 1)) continue;
            MapObject mo;
            mo.kind = MapObject::Kind::area, mo.cls = o.name, mo.x = static_cast<int>(ex), mo.y = static_cast<int>(ey), mo.owner = 14 + rescue_;
            objects.push_back(mo);
            if (game_start_)
                if (const auto span = start_respawn_span(props_, o.name)) rng_(0, *span);
            sample(ex - r, ey - r, ex + r, ey + r);
            const long long q = std::max(tdiv(r, 32), 1ll);
            stamp_diamond(mask, tdiv(o.dx, 32) + mx0, tdiv(o.dy, 32) + my0,
                          static_cast<long long>(std::trunc(static_cast<double>(q) * static_cast<double>(f32(1.42)))));
        }
        for (const auto& e : ar.decor) {
            const auto er = decor_rect(e.v & 0xFF);
            if (!er) continue;
            const long long ex = e.dx + xp, ey = e.dy + yp;
            const long long l = er->l + ex, t = er->t + ey, r_ = er->r + ex, b = er->b + ey;
            if (!(0 <= l && r_ <= size - 1 && 0 <= t && b <= size - 1)) continue;
            decor_.at(py_index(ey >> 6, decor_.rows), py_index(ex >> 6, decor_.cols)) = static_cast<std::uint16_t>(e.v & 0xFFFF);
            sample(l, t, r_, b);
            const long long l2 = tdiv(er->l + e.dx, 32), r2 = tdiv(er->r + e.dx, 32);
            const long long t2 = tdiv(er->t + e.dy, 32), b2 = tdiv(er->b + e.dy, 32);
            const long long q = std::max(tdiv(std::max(b2 - t2 + 1, r2 - l2 + 1), 2), 1ll);
            stamp_diamond(mask, tdiv(l2 + r2, 2) + mx0, tdiv(t2 + b2, 2) + my0,
                          static_cast<long long>(std::trunc(static_cast<double>(q) * static_cast<double>(f32(1.42)))));
        }
        float mean = count ? f32(static_cast<double>(total) / count) : total;
        if (static_cast<double>(mean) < static_cast<double>(f32(0.1))) mean = f32(0.1);
        long long xs = std::max(mx0, 0ll), xe = std::min(mx0 + (w64 >> 5), 512ll);
        long long ys = std::max(my0, 0ll), ye = std::min(my0 + (h64 >> 5), 512ll);
        const long long ox = tdiv(tdiv(w64, 2) + xp, 32) - 256, oy = tdiv(tdiv(h64, 2) + yp, 32) - 256;
        if (xs + ox < 0) xs = -ox;
        if (xe + ox >= 512) xe = 512 - ox;
        if (ys + oy < 0) ys = -oy;
        if (ye + oy >= 512) ye = 512 - oy;
        blend(mask, mean, xs, xe, ys, ye, ox, oy);
        return true;
    }

    // 0x00589860 (P12 claims 22-28).
    int spawn(Descriptor& d, std::pair<int, int> pt) {
        const int race = nearest_race(pt);
        rescue_ = d.rescue != 0 ? 1 : 0;
        if (d.setname.empty()) {
            const std::string cls = race_class(d, race);
            if (cls.rfind("Area::", 0) == 0) {
                const auto suffix = cls.substr(6);
                if (suffix.empty()) return 0;
                std::vector<int> hits;
                for (std::size_t i = 0; i < areas_.size() && hits.size() < 64; ++i)
                    if (areas_[i].name.rfind(suffix, 0) == 0) hits.push_back(static_cast<int>(i));
                if (hits.empty()) return 0;
                const int idx = hits[static_cast<std::size_t>(rng_(0, static_cast<std::int32_t>(hits.size()) - 1))];
                if (!project_area(idx, pt)) return 0;
                stamp(pt.first, pt.second, d.inf, d.fade);
                d.points.push_back(pt);
                return 1;
            }
            if (d.maxgroup > 1 && !cls.empty()) {
                group(d, pt, race, cls);
                return 0;
            }
            spawn_class(cls, pt);
            stamp(pt.first, pt.second, d.inf, d.fade);
            d.points.push_back(pt);
            return 1;
        }
        const int idx = select_template(rng_, lib_.records, std::string(kVillagePrefix[race]) + d.setname);
        if (idx < 0) return 0;
        spawn_decor(decor_, lib_, idx, pt, size_);
        flatten(a_, lib_, idx, pt, size_, 0.1);
        settlement_terrain_copy(cls_, lib_, idx, pt, size_);
        settlement_objects(objects, settlements, lib_, lo_, idx, pt, size_, 14 + rescue_, static_cast<int>(settlement_count()),
                           game_start_ ? &rng_ : nullptr, props_);
        settlements.back().adjust = true;  // AdjustSettlementPopulation after the PlaceObjects spawn
        last_obj_.reset();
        for (std::size_t i = objects.size(); i-- > 0;)
            if (objects[i].kind == MapObject::Kind::member) {
                last_obj_ = i;
                break;
            }
        for (const auto& [cx, cy] : lib_.records[static_cast<std::size_t>(idx)].comps) stamp(pt.first + cx, pt.second + cy, d.inf, d.fade);
        d.points.push_back(pt);
        return 1;
    }

    // 0x00589C20 (P12 claims 10-13; teleport path P17).
    int attempt(Descriptor& d, int flag) {
        last_obj_.reset();
        if (!d.cls.empty() && props_.heir(d.cls, "Teleport")) {
            // pairs of candidates at least TeleportMinDist % of the map width apart, up to 100 pair tries
            const long long dist = tdiv(static_cast<long long>(size_) * kTeleportMinDist, 100);
            std::pair<int, int> p1, p2;
            bool ok = false;
            for (int t = 0; t < 100; ++t) {
                const auto c1 = candidate(d);
                if (!c1) return 0;
                const auto c2 = candidate(d);
                if (!c2) return 0;
                p1 = *c1, p2 = *c2;
                const long long dy = p2.second - p1.second, dx = p2.first - p1.first;
                if (isqrt(dy * dy + dx * dx) >= dist) {
                    ok = true;
                    break;
                }
            }
            if (!ok) return 0;
            if (!spawn(d, p1)) return 0;
            const auto t1 = last_obj_;
            if (!spawn(d, p2)) throw std::runtime_error("not modelled: second teleport spawn failed (pair deletion never observed)");
            const auto t2 = last_obj_;
            // CVXTeleport::LinkPair 0x005CD910: each stores its partner; map.obj.xml writes it as destination_set
            objects.at(*t1).partner = objects.at(*t2).settlement;
            objects.at(*t2).partner = objects.at(*t1).settlement;
            return 1;
        }
        int r;
        if (flag && d.benefit > 0) {
            std::vector<std::pair<int, int>> cands;
            for (int i = 0; i < 100; ++i)
                if (const auto c = candidate(d)) cands.push_back(*c);
            if (cands.empty()) return 0;
            bool have = false;
            double best = 0.0;
            std::size_t bi = 0;
            std::vector<float> bvec;
            for (std::size_t i = 0; i < cands.size(); ++i) {
                auto v = add_benefit(vec_, cands[i], d.benefit);
                const double s = cv(v);
                if (!have || s < best) have = true, best = s, bi = i, bvec = v;
            }
            vec_ = bvec;
            r = spawn(d, cands[bi]);
        } else {
            const auto pt = candidate(d);
            if (!pt) return 0;
            r = spawn(d, *pt);
            if (r) vec_ = add_benefit(vec_, *pt, d.benefit);
        }
        if (r && last_obj_ && objects.at(*last_obj_).settlement >= 0) {
            // the last created building adds its class house_pop_bonus (+0xAD4) to its settlement's max population
            const auto v = props_.prop(objects.at(*last_obj_).cls, "house_pop_bonus");
            const long long bonus = v && !v->empty() ? rmdata::py_int(*v) : 0;
            settlement_at(static_cast<std::size_t>(objects.at(*last_obj_).settlement)).pop_bonus = bonus;
        }
        return r;
    }
};

}  // namespace iberivm::rm
