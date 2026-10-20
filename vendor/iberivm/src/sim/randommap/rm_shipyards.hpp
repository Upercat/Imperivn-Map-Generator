// Shipyard pass 0x0058C810 and the settlement-library stamp 0x00590010 with its height relaxation
// (reference: tools/randommap/shipyards.py; cerebrivm function:random-map-place-shipyards,
// function:random-map-settlement-library-stamp-template, function:terrain-relax-heights-in-dirty-rect,
// function:terrain-relax-height-queue-cardinal, function:terrain-commit-relaxed-heights).
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include "randommap_data.hpp"
#include "randommap_props.hpp"
#include "rm_common.hpp"
#include "rm_fields.hpp"
#include "rm_library.hpp"
#include "rm_objects.hpp"

namespace iberivm::rm {

// CONST.INI [RandomMap]
constexpr int kMinLakeIslandArea = 1024, kSyMinDistSameBound = 48, kSyMinDistDiffBound = 48;
constexpr int kShipyardOwner = 15;  // slot argument 0x10 - 1; written as player="16"
constexpr int kLibMax = 32767;      // library terrain bounds (CSettlementTemplateLib +0x7C/+0x80)
constexpr int kWater = 13;
// 0x0058CE98: the shuffle can swap v[s] (one past the end) into the list; under the debug heap of the
// captures the unwritten vector tail reads 0xBAADF00D (claim:random-map-shipyard-shuffle-can-swap-past-end-element-into-list).
constexpr long long kPastEnd = -0x45521FF3ll;

// Python slice [start, stop) over a dimension of length n.
inline std::pair<long long, long long> py_slice(long long start, long long stop, long long n) {
    auto fix = [n](long long v) {
        if (v < 0) v += n;
        return std::clamp(v, 0ll, n);
    };
    return {fix(start), fix(stop)};
}

struct ShipyardData {
    std::vector<std::string> roman, gaul;
    const GameFS* fs = nullptr;
    rmdata::ClassData* classes = nullptr;
    std::map<std::string, std::vector<std::tuple<int, int, int>>> footprints;

    // Entity points (+0x33C) of type 5 (water) and 6 (land), read on first use like shipyards.footprint.
    const std::vector<std::tuple<int, int, int>>& footprint(const std::string& c) {
        if (const auto it = footprints.find(c); it != footprints.end()) return it->second;
        const auto ent = classes->class_entity(c);
        if (!ent) throw std::runtime_error("shipyard class without entity: " + c);
        const auto t = fs->read_text(*ent);
        std::vector<std::tuple<int, int, int>> pts;
        for (std::size_t p = 0;;) {  // re.findall(r"<point\s(.*?)/>", t, re.S)
            const auto b = t.find("<point", p);
            if (b == std::string::npos) break;
            p = b + 1;
            if (b + 6 >= t.size()) break;
            const char ch = t[b + 6];
            if (!(ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f' || ch == '\v')) continue;
            const auto e = t.find("/>", b + 7);
            if (e == std::string::npos) break;
            const auto a = xml::regex_attrs(t.substr(b + 7, e - b - 7));
            p = e + 2;
            const auto* ty = xml::regex_get(a, "type");
            if (ty && (*ty == "5" || *ty == "6"))
                pts.emplace_back(static_cast<int>(rmdata::py_int(*xml::regex_get(a, "x"))),
                                 static_cast<int>(rmdata::py_int(*xml::regex_get(a, "y"))), static_cast<int>(rmdata::py_int(*ty)));
        }
        return footprints.emplace(c, std::move(pts)).first->second;
    }
};

// 0x0058BA30/0x0058B6C0: BaseShipyard heirs in strcmp order, ids starting with 'R' (Roman) and 'G' (Gaul).
inline ShipyardData load_shipyard_data(const GameFS& fs, rmdata::ClassProps& props, rmdata::ClassData& classes) {
    ShipyardData d;
    std::vector<std::string> ids;
    for (const auto& f : fs.list_files("DATA/CLASSES")) {
        const auto t = fs.read_text(f);
        std::size_t b = 0;
        for (;;) {
            b = t.find("<class", b);
            if (b == std::string::npos || b + 6 >= t.size()) break;
            const char c = t[b + 6];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') break;
            ++b;
        }
        if (b == std::string::npos || b + 6 >= t.size()) continue;
        const auto e = t.find('>', b + 7);
        if (e == std::string::npos) continue;
        const auto a = xml::regex_attrs(t.substr(b + 7, e - b - 7));
        const auto* id = xml::regex_get(a, "id");
        if (id && !id->empty() && props.heir(*id, "BaseShipyard")) ids.push_back(*id);
    }
    std::sort(ids.begin(), ids.end(), [](const std::string& x, const std::string& y) {
        return std::lexicographical_compare(x.begin(), x.end(), y.begin(), y.end(),
                                            [](char p, char q) { return static_cast<unsigned char>(p) < static_cast<unsigned char>(q); });
    });
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    for (const auto& c : ids) {
        if (c[0] == 'R') d.roman.push_back(c);
        if (c[0] == 'G') d.gaul.push_back(c);
    }
    d.fs = &fs;
    d.classes = &classes;
    return d;
}

// 0x0058BBF0: 4-neighbour fill of the component holding g[y, x] with label; returns its cell count.
inline long long label_fill(Grid<std::int32_t>& g, int x, int y, std::int32_t label) {
    const int n = g.rows;
    const auto old = g.at(y, x);
    g.at(y, x) = label;
    std::deque<std::pair<int, int>> q{{x, y}};
    long long count = 1;
    while (!q.empty()) {
        const auto [cx, cy] = q.front();
        q.pop_front();
        const int nb[4][2] = {{cx, cy - 1}, {cx - 1, cy}, {cx + 1, cy}, {cx, cy + 1}};
        for (const auto& p : nb)
            if (0 <= p[0] && p[0] < n && 0 <= p[1] && p[1] < n && g.at(p[1], p[0]) == old) {
                g.at(p[1], p[0]) = label;
                q.emplace_back(p[0], p[1]);
                ++count;
            }
    }
    return count;
}

inline bool is_land_label(long long v) { return 0x8000 <= v && v <= 0xBFFF; }

// Global relaxation buffers 0x009BDA80..: H heights, F lock flags, C visit counters per 32-unit cell.
struct Relax {
    Grid<std::uint8_t>& h;
    Grid<std::int32_t> H, F, C;
    explicit Relax(Grid<std::uint8_t>& heights) : h(heights), H(heights.rows, heights.cols), F(heights.rows, heights.cols, 0), C(heights.rows, heights.cols, 0) {
        for (std::size_t i = 0; i < h.v.size(); ++i) H.v[i] = h.v[i];  // 0x00544FD0 via 0x0058DAB0
    }

    // 0x00548830: grow by 0xA0, clip, queue every cell (x outer), then 0x00548070.
    void run(long long rect[4], int size) {
        rect[0] -= 0xA0, rect[1] -= 0xA0, rect[2] += 0xA0, rect[3] += 0xA0;
        rect[0] = std::max(rect[0], 0ll), rect[1] = std::max(rect[1], 0ll);
        rect[2] = std::min(rect[2], static_cast<long long>(size - 1)), rect[3] = std::min(rect[3], static_cast<long long>(size - 1));
        std::deque<std::pair<int, int>> q;  // push_front / pop_back: FIFO
        for (long long cx = tdiv(rect[0], 32); cx <= tdiv(rect[2], 32); ++cx)
            for (long long cy = tdiv(rect[1], 32); cy <= tdiv(rect[3], 32); ++cy) q.emplace_back(static_cast<int>(cy), static_cast<int>(cx));
        const int nh = H.rows, nw = H.cols;
        while (!q.empty()) {
            const auto [y, x] = q.front();
            q.pop_front();
            const int hv = H.at(y, x);
            bool changed = false;
            const int nbs[3][4] = {{y - 1, x, y - 1 >= 0, 1}, {y + 1, x, y + 1 < nh, 3}, {y, x - 1, x - 1 >= 0, 0}};
            for (const auto& e : nbs) {
                if (!e[2]) continue;
                const int ny = e[0], nx = e[1], side = e[3];
                const int a = H.at(ny, nx);
                if (std::abs(a - hv) > 20) {
                    if (F.at(ny, nx) != 1 || F.at(y, x) == 1) {
                        if (side == 1) rect[1] = std::min(rect[1], static_cast<long long>(y) * 32 - 32);
                        else if (side == 3) rect[3] = std::max(rect[3], static_cast<long long>(y + 1) * 32);
                        else rect[0] = std::min(rect[0], static_cast<long long>(x) * 32 - 32);
                        H.at(ny, nx) = a > hv ? hv + 20 : hv - 20;
                        F.at(ny, nx) = 0xFF;
                    }
                    q.emplace_back(ny, nx);
                    changed = true;
                }
            }
            bool east = false;
            if (x + 1 < nw && F.at(y, x + 1) != 1) {
                const int a = H.at(y, x + 1);
                if (std::abs(a - hv) > 20) {
                    rect[2] = std::max(rect[2], static_cast<long long>(x + 1) * 32);
                    H.at(y, x + 1) = a > hv ? hv + 20 : hv - 20;
                    F.at(y, x + 1) = 0xFF;
                    q.emplace_back(y, x + 1);
                    east = true;
                }
            }
            if (changed || east) {
                q.emplace_back(y, x);
                if (F.at(y, x) == 1) {
                    const int c = C.at(y, x);
                    C.at(y, x) = (c + 1) & 0xFF;
                    if (c == 32) F.at(y, x) = 0xFF;
                }
            }
        }
        const auto [y0, y1] = py_slice(tdiv(rect[1], 32), tdiv(rect[3], 32) + 1, C.rows);
        const auto [x0, x1] = py_slice(tdiv(rect[0], 32), tdiv(rect[2], 32) + 1, C.cols);
        for (long long yy = y0; yy < y1; ++yy)
            for (long long xx = x0; xx < x1; ++xx) C.at(static_cast<int>(yy), static_cast<int>(xx)) = 0;
    }

    // 0x00549810(rect, 0): H back into the height grid over the rect cells.
    void commit(const long long rect[4]) {
        const auto [y0, y1] = py_slice(tdiv(rect[1], 32), tdiv(rect[3], 32) + 1, h.rows);
        const auto [x0, x1] = py_slice(tdiv(rect[0], 32), tdiv(rect[2], 32) + 1, h.cols);
        for (long long yy = y0; yy < y1; ++yy)
            for (long long xx = x0; xx < x1; ++xx)
                h.at(static_cast<int>(yy), static_cast<int>(xx)) = static_cast<std::uint8_t>(H.at(static_cast<int>(yy), static_cast<int>(xx)));
    }
};

// 0x005468D0: on class 13, not all water around (0x005467E0); otherwise a class-13 cell within the disc.
inline bool near_shore(const Grid<std::uint8_t>& cls, long long px, long long py, int size) {
    auto at = [&](long long x, long long y) { return static_cast<int>(cls.at(py_index(y >> 6, cls.rows), py_index(x >> 6, cls.cols))); };
    const bool on_water = at(px, py) == kWater;
    for (int dx = -4; dx <= 4; ++dx)
        for (int dy = -4; dy <= 4; ++dy) {
            if (!(dx || dy) || dx * dx + dy * dy > 16) continue;
            const long long x = px + dx * 64, y = py + dy * 64;
            if (!(0 <= x && x <= size - 1 && 0 <= y && y <= size - 1)) continue;
            const int v = at(x, y);
            if (on_water ? (v != 12 && v != 13) : v == kWater) return true;
        }
    return false;
}

struct ShipyardContext {
    int size;
    Grid<std::uint8_t>& cls;
    Grid<std::uint8_t>& h;
    Grid<std::uint16_t>& decor;
    std::vector<MapObject>& objects;
    std::vector<Settlement>& settlements;
    const Library& lib;
    const LibraryObjects& lo;
    rmdata::ClassProps& props;
    DrawLog& rng;
    std::map<int, std::vector<std::pair<int, int>>> by_race;  // stronghold anchors per race (slot order)
};

// 0x0057DE00 (claim:random-map-nearest-stronghold-race-by-point-lists).
inline int nearest_race_of(const std::map<int, std::vector<std::pair<int, int>>>& by_race, std::pair<long long, long long> pt) {
    long long best = 0xFFFFFFFFll;
    int br = 0;
    for (int r = 0; r < 8; ++r) {
        const auto it = by_race.find(r);
        if (it == by_race.end()) continue;
        for (const auto& [x, y] : it->second) {
            const long long dx = pt.first - x, dy = pt.second - y;
            const long long dd = isqrt((dx * dx + dy * dy) & 0xFFFFFFFFll);
            if (dd < best) best = dd, br = r;
        }
    }
    return br;
}

// 0x00590010(lib; -1, name, x, y, owner+1, spawn 1, refresh 0).
inline void shipyard_stamp(ShipyardContext& st, Relax& relax, const std::string& name, long long x, long long y, int owner) {
    const int size = st.size;
    int idx = -1;
    for (std::size_t i = 0; i < st.lib.records.size(); ++i)
        if (st.lib.records[i].name == name) {
            idx = static_cast<int>(i);
            break;
        }
    if (idx < 0) throw std::runtime_error("settlement library has no record " + name);
    const auto& rec = st.lib.records[static_cast<std::size_t>(idx)];
    const long long hw = tdiv(rec.w, 2), hh = tdiv(rec.h, 2);
    x = x < hw + 1 ? hw + 1 : (size <= hw + 1 + x ? size - hw - 2 : x);
    y = y < hh + 1 ? hh + 1 : (size <= hh + 1 + y ? size - hh - 2 : y);
    const int sid = static_cast<int>(st.settlements.size());
    Settlement s;
    s.record = true, s.idx = idx, s.owner = owner, s.x = static_cast<int>(x), s.y = static_cast<int>(y),
    s.attrs = &st.lo.recs[static_cast<std::size_t>(idx)].head;
    st.settlements.push_back(s);
    long long L = x, T = y, R = x, B = y;
    for (std::size_t m = 0; m < rec.comps.size(); ++m) {
        const auto [dx, dy] = rec.comps[m];
        MapObject o;
        o.kind = MapObject::Kind::member, o.cls = rec.classes[m], o.x = static_cast<int>(x + dx), o.y = static_cast<int>(y + dy),
        o.settlement = sid, o.owner = owner;
        st.objects.push_back(o);
        const auto er = st.lib.classes->class_rect(rec.classes[m]);
        if (er) L = er->l + dx + x, T = er->t + dy + y, R = er->r + dx + x, B = er->b + dy + y;
    }
    L -= 0x100, T -= 0x100, R += 0x100, B += 0x100;
    const long long u = isqrt(((T - B) * (T - B) + (L - R) * (L - R)) & 0xFFFFFFFFll);
    clear_decor_disc(st.decor, static_cast<int>(tdiv(L + R, 2)), static_cast<int>(tdiv(T + B, 2)), tdiv(u >> 1, 5) + (u >> 1), size);
    const auto& lt = st.lib.terrain;
    auto lock = [&](long long px, long long py) {
        if (0x20 < py && py < size - 1 - 0x20 && 0x20 < px && px < size - 1 - 0x20) {
            const auto [y0, y1] = py_slice(tdiv(py, 32), tdiv(py + 64, 32), relax.F.rows);
            const auto [x0, x1] = py_slice(tdiv(px, 32), tdiv(px + 64, 32), relax.F.cols);
            for (long long yy = y0; yy < y1; ++yy)
                for (long long xx = x0; xx < x1; ++xx) {
                    relax.F.at(static_cast<int>(yy), static_cast<int>(xx)) = 1;
                    relax.H.at(static_cast<int>(yy), static_cast<int>(xx)) = 0;
                }
        }
    };
    auto cls_at = [&](long long px, long long py) -> std::uint8_t& {
        return st.cls.at(py_index(py >> 6, st.cls.rows), py_index(px >> 6, st.cls.cols));
    };
    auto lt_at = [&](long long lx, long long ly) { return static_cast<int>(lt.at(py_index(ly >> 6, lt.rows), py_index(lx >> 6, lt.cols))); };
    // 0x00590520..0x0059075F: library classes over the rect grown by 0x100 (not clipped)
    for (long long py = T; py < B; py += 64) {
        const long long ly = py - y + rec.cy;
        for (long long px = L; px < R; px += 64) {
            const long long lx = px + rec.cx - x;
            if (lx < 0 || lx > kLibMax || ly < 0 || ly > kLibMax)
                cls_at(px, py) = 0x0F;
            else {
                const int v = lt_at(lx, ly);
                if (v != 0x0F) {
                    cls_at(px, py) = static_cast<std::uint8_t>(v);
                    lock(px, py);
                }
            }
        }
    }
    // 0x00590878..0x005909A1: transparent or outside cells near water become class 0xC over the rect grown by 0x300
    L = std::max(L - 0x200, 0ll), T = std::max(T - 0x200, 0ll), R = std::min(R + 0x200, static_cast<long long>(size - 1)),
    B = std::min(B + 0x200, static_cast<long long>(size - 1));
    for (long long py = T; py < B; py += 64) {
        const long long ly = py - y + rec.cy;
        for (long long px = L; px < R; px += 64) {
            const long long lx = px + rec.cx - x;
            if (lx < 1 || lx > kLibMax || ly < 1 || ly > kLibMax || lt_at(lx, ly) == 0x0F)
                if (near_shore(st.cls, px, py, size)) {
                    cls_at(px, py) = 0x0C;
                    lock(px, py);
                }
        }
    }
    long long rect[4] = {L - 0x80, T - 0x80, R + 0x80, B + 0x80};
    relax.run(rect, size);
    relax.commit(rect);
    // 0x005496E0 over the commit rect grown by 0x40: non-area objects without importsettlement whose water_unit flag
    // disagrees with class 13 under them are deleted
    const long long dr[4] = {std::max(rect[0] - 0x40, 0ll), std::max(rect[1] - 0x40, 0ll), std::min(rect[2] + 0x40, static_cast<long long>(size - 1)),
                             std::min(rect[3] + 0x40, static_cast<long long>(size - 1))};
    std::vector<MapObject> kept;
    for (const auto& o : st.objects) {
        const auto imp = st.props.prop(o.cls, "importsettlement");
        if (dr[0] <= o.x && o.x <= dr[2] && dr[1] <= o.y && o.y <= dr[3] && o.cls != "AdvArea" && !(imp && !imp->empty())) {
            const bool water = st.cls.at(py_index(o.y >> 6, st.cls.rows), py_index(o.x >> 6, st.cls.cols)) == kWater;
            const auto wu = st.props.prop(o.cls, "water_unit");
            const bool water_unit = (wu && !wu->empty() ? rmdata::py_int(*wu) : 0) != 0;
            if (water_unit != water) {
                if (o.kind == MapObject::Kind::member)
                    throw std::runtime_error("not modelled: settlement member deleted by 0x005496E0 (" + o.cls + ")");
                continue;
            }
        }
        kept.push_back(o);
    }
    st.objects = std::move(kept);
}

// 0x0058C810
inline void place_shipyards(ShipyardContext& st, ShipyardData& sd) {
    const int size = st.size;
    const int n = static_cast<int>(tdiv(size, 64));
    Grid<std::int32_t> g(n, n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) g.at(y, x) = st.cls.at(y, x) != kWater ? 1 : 0;
    std::int32_t water = 0x4000, land = 0x8000;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const auto v = g.at(y, x);
            if (v == 0) {
                const auto c = label_fill(g, x, y, water++);
                if (c < kMinLakeIslandArea) label_fill(g, x, y, land++);
            } else if (v == 1) {
                const auto c = label_fill(g, x, y, land++);
                if (c < kMinLakeIslandArea) label_fill(g, x, y, water++);
            }
        }
    using Key = std::pair<std::int32_t, std::int32_t>;
    std::map<Key, std::vector<std::pair<long long, long long>>> lists;
    for (int y = 0; y < n - 1; ++y)
        for (int x = 0; x < n - 1; ++x) {
            const auto a = g.at(y, x);
            for (const auto b : {g.at(y, x + 1), g.at(y + 1, x)})
                if (a != b && is_land_label(a) != is_land_label(b)) {
                    lists[is_land_label(a) ? Key{a, b} : Key{b, a}].emplace_back(x, y);
                    break;
                }
        }
    std::vector<Key> keys;
    for (const auto& [k, v] : lists) keys.push_back(k);
    for (std::size_t k = 0; k < keys.size(); ++k)
        for (const auto& [x, y] : lists[keys[k]]) g.at(static_cast<int>(y), static_cast<int>(x)) = static_cast<std::int32_t>(0xC000 + k);
    for (const auto& key : keys) {
        auto& v = lists[key];
        for (std::size_t i = 1; i < v.size(); ++i) {
            const auto j = static_cast<std::size_t>(st.rng(0, static_cast<std::int32_t>(i + 1)));
            if (j == v.size()) v[i] = {kPastEnd, kPastEnd};
            else std::swap(v[i], v[j]);
        }
    }
    Relax relax(st.h);
    auto select = [&](std::pair<long long, long long> pt) -> std::optional<std::string> {  // 0x0058C0F0 with 0x0058A7F0
        const long long px = pt.first * 64, py = pt.second * 64;
        if (!(0x300 <= px && px <= size - 1 - 0x300 && 0x300 <= py && py <= size - 1 - 0x300)) return std::nullopt;
        const int race = nearest_race_of(st.by_race, {px, py});
        if (race > 7) return std::nullopt;
        std::vector<std::string> order;
        const auto& first = (race == 0 || race == 3) ? sd.gaul : sd.roman;
        const auto& second = (race == 0 || race == 3) ? sd.roman : sd.gaul;
        order.insert(order.end(), first.begin(), first.end());
        order.insert(order.end(), second.begin(), second.end());
        for (const auto& c : order) {
            bool ok = true;
            for (const auto& [fx, fy, t] : sd.footprint(c)) {
                const long long lab = g.at(py_index(tdiv(py + fy, 64), g.rows), py_index(tdiv(px + fx, 64), g.cols));
                if ((t == 5 && !(0x4000 <= lab && lab < 0x8000)) || (t == 6 && !(0x8000 <= lab && lab < 0xC000))) {
                    ok = false;
                    break;
                }
            }
            if (ok) return c;
        }
        return std::nullopt;
    };
    for (;;) {
        std::size_t best = 0;
        std::optional<Key> key;
        for (const auto& k : keys)
            if (lists[k].size() > best) best = lists[k].size(), key = k;
        if (!key) break;
        auto& v = lists[*key];
        std::optional<std::string> c;
        std::pair<long long, long long> pt{};
        while (!v.empty() && !c) {
            pt = v.back();
            v.pop_back();
            c = select(pt);
        }
        if (v.empty()) continue;  // a success on a list's last candidate is discarded
        const auto imp = st.props.prop(*c, "importsettlement");
        if (!imp) throw std::runtime_error("shipyard class without importsettlement: " + *c);
        shipyard_stamp(st, relax, *imp, pt.first * 64, pt.second * 64, kShipyardOwner);
        const long long r = st.rng(0, static_cast<std::int32_t>(tdiv(2 * kSyMinDistSameBound, 4)));
        for (const auto& k : keys) {
            const long long rad = k == *key ? tdiv(3 * kSyMinDistSameBound, 4) + r : kSyMinDistDiffBound;
            auto& lk = lists[k];
            std::vector<std::pair<long long, long long>> keep;
            for (const auto& p : lk) {
                const long long dx = p.first - pt.first, dy = p.second - pt.second;
                if (isqrt((dx * dx + dy * dy) & 0xFFFFFFFFll) >= rad) keep.push_back(p);
            }
            lk = std::move(keep);
        }
    }
}

}  // namespace iberivm::rm
