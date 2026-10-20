// Fields and settlement stamps before the influence grid (reference: exact.template_field,
// exact.field_a, exact.macrozones, exact.corridor, exact.flatten, exact.spawn_decor,
// exact.settlement_terrain_copy, exact.influence_grid and influence.py).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <vector>
#include "rm_common.hpp"
#include "rm_library.hpp"
#include "rm_noise.hpp"
#include "rm_template.hpp"

namespace iberivm::rm {

// numpy-style index: negative values count from the end, values past the end are an error.
inline int py_index(long long i, int n) {
    if (i < 0) i += n;
    if (i < 0 || i >= n) throw std::runtime_error("index out of range in random-map stage");
    return static_cast<int>(i);
}

// 0x005CE580 fill (copy for 512, 2x2 mean for 256, clamped bicubic upsample for 1024), 0x00581140
// x0.75f, two in-place 7x7 means (claim:random-map-template-field-is-scaled-land-mask-smoothed-twice-7x7).
inline Grid<float> template_field(const Grid<std::uint8_t>& semantic, int size) {
    Grid<float> land(TN, TN);
    for (std::size_t i = 0; i < land.v.size(); ++i) land.v[i] = semantic.v[i] != 0 ? 1.0f : 0.0f;
    const int n = size >> 5;
    Grid<float> t;
    if (n == 512) {
        t = land;
    } else if (n == 256) {
        t = Grid<float>(256, 256);
        const double q = f32(0.25);
        for (int r = 0; r < 256; ++r)
            for (int c = 0; c < 256; ++c) {
                const double s = ((static_cast<double>(land.at(2 * r, 2 * c)) + land.at(2 * r, 2 * c + 1)) + land.at(2 * r + 1, 2 * c)) +
                                 land.at(2 * r + 1, 2 * c + 1);
                t.at(r, c) = f32(s * q);
            }
    } else if (n == 1024) {
        Amps amp{};
        const int L = log2_field(n);
        amp[static_cast<std::size_t>(L - 1)] = 1.0f;
        std::map<int, const Grid<float>*> user{{L - 1, &land}};
        t = noise_generate(n, amp, 0, &user);
        for (auto& x : t.v) x = std::clamp(x, 0.0f, 1.0f);
    } else
        throw std::runtime_error("template field side " + std::to_string(n));
    const double s75 = f32(0.75);
    for (auto& x : t.v) x = f32(static_cast<double>(x) * s75);
    box_inplace(t, 3);
    box_inplace(t, 3);
    return t;
}

// 0x00582064..0x00582163 (claim:random-map-field-a-is-renormalised-quantised-noise-times-template-field).
inline Grid<float> field_a(std::uint32_t seed, const Grid<float>& t) {
    const int n = t.rows;
    const int L = log2_field(n);
    const auto q = quantise(noise_normalise(noise_generate(n, set_amps(L - 6, 3, 0.75), seed)));
    Grid<float> prod(n, n);
    for (std::size_t i = 0; i < prod.v.size(); ++i) prod.v[i] = f32(static_cast<double>(q.v[i]) * static_cast<double>(t.v[i]));
    return noise_normalise(prod);
}

// 0x0057E090 (claims: random-map-macrozone-base-grid-stores-nearest-race-modulo-4,
// random-map-macrozone-warp-resamples-base-grid-by-truncation).
inline Grid<std::uint8_t> macrozones(int size, const std::map<int, std::vector<std::pair<int, int>>>& by_race,
                                     std::pair<std::uint32_t, std::uint32_t> seeds) {
    const int n = size / 128;
    std::map<int, Grid<long long>> per_race;
    for (const auto& [race, pts] : by_race)
        for (const auto& [x, y] : pts) {
            auto it = per_race.find(race);
            if (it == per_race.end()) it = per_race.emplace(race, Grid<long long>(n, n, 0xFFFFFFFFll)).first;
            for (int yy = 0; yy < n; ++yy)
                for (int xx = 0; xx < n; ++xx) {
                    const long long dx = static_cast<long long>(xx) * 128 - x, dy = static_cast<long long>(yy) * 128 - y;
                    const auto d = static_cast<long long>(std::floor(std::sqrt(static_cast<double>(dx * dx + dy * dy))));
                    auto& cell = it->second.at(yy, xx);
                    cell = std::min(cell, d);
                }
        }
    Grid<long long> best(n, n, 0xFFFFFFFFll), choice(n, n, 0);
    for (int r = 0; r < 8; ++r) {
        const auto it = per_race.find(r);
        if (it == per_race.end()) continue;
        for (std::size_t i = 0; i < best.v.size(); ++i)
            if (it->second.v[i] < best.v[i]) best.v[i] = it->second.v[i], choice.v[i] = r;
    }
    const auto fx = noise_normalise(noise_generate(n, set_amps(4, 2, 0.5), seeds.first));
    const auto fy = noise_normalise(noise_generate(n, set_amps(4, 2, 0.5), seeds.second));
    const float inv = 1.0f / static_cast<float>(n * 0.5);
    std::vector<double> tap(static_cast<std::size_t>(n), 1.0);
    for (int i = 0; i < n; ++i) {
        const float fi = static_cast<float>(i);
        if (fi < n * 0.5) tap[static_cast<std::size_t>(i)] = static_cast<double>(fi) * static_cast<double>(inv);
        if (fi > n - n * 0.5) tap[static_cast<std::size_t>(i)] = static_cast<double>(static_cast<float>(n - fi)) * static_cast<double>(inv);
    }
    Grid<std::uint8_t> out(n, n);
    for (int yy = 0; yy < n; ++yy)
        for (int xx = 0; xx < n; ++xx) {
            const float A = f32(static_cast<double>(fx.at(yy, xx) - 0.5f) * tap[static_cast<std::size_t>(xx)]);
            const float Bv = f32(static_cast<double>(fy.at(yy, xx) - 0.5f) * tap[static_cast<std::size_t>(yy)]);
            double X = static_cast<double>(f32(xx + n * static_cast<double>(A) * 0.5));
            double Y = yy + n * static_cast<double>(Bv) * 0.5;
            X = std::clamp(X, 0.0, static_cast<double>(n - 1));
            Y = std::clamp(Y, 0.0, static_cast<double>(n - 1));
            const long long sx = static_cast<long long>(std::trunc(X * 128)) >> 7;
            const long long sy = static_cast<long long>(std::trunc(Y * 128)) >> 7;
            out.at(yy, xx) = static_cast<std::uint8_t>(choice.at(static_cast<int>(sy), static_cast<int>(sx)) & 3);
        }
    return out;
}

// 0x0057B360 (claim:random-map-settlement-corridor-raises-height-to-water-level).
inline void corridor(Grid<float>& a, int x1, int y1, int x2, int y2, double h, int hw) {
    if (std::min({x1, y1, x2, y2}) < 0) return;
    const int n = a.rows;
    const float hv = f32(h);
    const long long dx = x2 - x1, dy = y2 - y1;
    auto brush = [&](long long px, long long py) {
        const long long cx = tdiv(px, 32), cy = tdiv(py, 32);
        const long long r0 = std::max(cy - hw, 0ll), r1 = std::min(cy + hw, static_cast<long long>(n - 1));
        const long long c0 = std::max(cx - hw, 0ll), c1 = std::min(cx + hw, static_cast<long long>(n - 1));
        if (r0 <= r1 && c0 <= c1)
            for (long long r = r0; r <= r1; ++r)
                for (long long c = c0; c <= c1; ++c) {
                    auto& cell = a.at(static_cast<int>(r), static_cast<int>(c));
                    cell = std::max(cell, hv);
                }
    };
    if (std::llabs(dx) >= std::llabs(dy) && dx != 0) {
        const double s = f32(static_cast<double>(dy) / static_cast<double>(dx));
        const int step = dx > 0 ? 1 : -1;
        for (long long x = x1; x != x2; x += step) brush(x, static_cast<long long>(std::trunc(y1 + (x - x1) * s)));
    } else if (dy != 0) {
        const double s = f32(static_cast<double>(dx) / static_cast<double>(dy));
        const int step = dy > 0 ? 1 : -1;
        for (long long y = y1; y != y2; y += step) brush(static_cast<long long>(std::trunc(x1 + (y - y1) * s)), y);
    }
}

// 0x0058E3C0 steps 3 and 7, read at 0x0058E9F6..0x0058EBAB
// (claim:random-map-settlement-spawn-flattens-height-to-one-floored-mean).
inline void flatten(Grid<float>& a, const Library& lib, int idx, std::pair<int, int> anchor, int size, double h = 0.1) {
    const auto& rec = lib.records.at(static_cast<std::size_t>(idx));
    const auto [xo, yo] = project(rec, anchor.first, anchor.second, size);
    const int n = a.rows;
    const long long ox = tdiv(xo, 32), oy = tdiv(yo, 32);
    Grid<std::uint8_t> mask(512, 512, 0);
    float total = 0.0f;
    int count = 0;
    for (std::size_t m = 0; m < rec.comps.size(); ++m) {
        const auto [cx, cy] = rec.comps[m];
        const auto cr = lib.class_rect(rec.classes[m]);
        const long long rl = tdiv(cr.l + cx, 32) - 8, rt = tdiv(cr.t + cy, 32) - 8;
        const long long rr = tdiv(cr.r + cx, 32) + 8, rb = tdiv(cr.b + cy, 32) + 8;
        const long long r = static_cast<long long>(std::trunc(static_cast<double>(tdiv(std::max(rr - rl + 1, rb - rt + 1), 2)) *
                                                              static_cast<double>(f32(1.42))));
        const long long mx = tdiv(rl + rr, 2) + 255, my = tdiv(rt + rb, 2) + 255;
        for (long long i = 0; i <= r; ++i)
            for (long long j = 0; j <= r - i; ++j) {
                const long long d = i + j;
                const std::uint8_t v = d < r - 4 ? 0xFF : (d < r - 2 ? 0xC0 : (d < r ? 0x80 : 0x40));
                const long long pos[4][2] = {{my + i, mx + j}, {my + i, mx - j}, {my - i, mx + j}, {my - i, mx - j}};
                for (const auto& p : pos) mask.at(py_index(p[0], 512), py_index(p[1], 512)) |= v;
            }
        const long long xc = tdiv(xo + cx, 32), yc = tdiv(yo + cy, 32);
        auto A = [&](long long r_, long long c_) { return a.at(py_index(r_, n), py_index(c_, n)); };
        const float samples[5] = {A(yc, xc), A(yc, std::max(rl + ox, 0ll)), A(std::max(rt + oy, 0ll), xc),
                                  A(yc, std::min(rr + ox, static_cast<long long>(n - 1))),
                                  A(std::min(rb + oy, static_cast<long long>(n - 1)), xc)};
        double acc = total;
        for (float s : samples) acc += static_cast<double>(s);  // x87 extended accumulation, one FSTP per component
        total = f32(acc);
        count += 5;
    }
    float mean = f32(static_cast<double>(total) / count);
    if (static_cast<double>(mean) < static_cast<double>(f32(h))) mean = f32(h);
    const double inv = f32(1.0 / 255.0);
    for (int row = 0; row < 512; ++row)
        for (int col = 0; col < 512; ++col) {
            const int m = mask.at(row, col);
            if (!m) continue;
            const long long y = oy + row - 255, x = ox + col - 255;
            if (0 <= y && y < n && 0 <= x && x < n) {
                auto& cell = a.at(static_cast<int>(y), static_cast<int>(x));
                cell = f32(((255 - m) * static_cast<double>(cell) + m * static_cast<double>(mean)) * inv);
            }
        }
}

// 0x0058DB30: 64-pixel cells, strict disc, exclusive upper bound
// (claim:random-map-settlement-decor-clear-radius-from-bounding-box-diagonal).
inline int clear_decor_disc(Grid<std::uint16_t>& decor, int cx, int cy, long long r, int size) {
    const long long ccx = tdiv(cx, 64), ccy = tdiv(cy, 64), rc = tdiv(r + 63, 64);
    const long long lim = tdiv(size - 1, 64);
    int n = 0;
    for (long long y = std::max(ccy - rc, 0ll); y < std::min(ccy + rc + 1, lim); ++y)
        for (long long x = std::max(ccx - rc, 0ll); x < std::min(ccx + rc + 1, lim); ++x)
            if ((x - ccx) * (x - ccx) + (y - ccy) * (y - ccy) < rc * rc) {
                auto& cell = decor.at(static_cast<int>(y), static_cast<int>(x));
                if (cell) cell = 0, ++n;
            }
    return n;
}

// 0x0058E3C0 decor steps: clear the disc, then write the record's decor entries (P13).
inline void spawn_decor(Grid<std::uint16_t>& decor, const Library& lib, int idx, std::pair<int, int> anchor, int size) {
    const auto& r = lib.records.at(static_cast<std::size_t>(idx));
    const auto [xo, yo] = project(r, anchor.first, anchor.second, size);
    const long long d = isqrt(static_cast<long long>(r.w - 1) * (r.w - 1) + static_cast<long long>(r.h - 1) * (r.h - 1));
    const long long radius = (d >> 1) + tdiv(d >> 1, 5);
    clear_decor_disc(decor, xo, yo, radius, size);
    for (const auto& e : lib.decor_entries[static_cast<std::size_t>(idx)]) {
        const int x2 = xo + e.dx, y2 = yo + e.dy;
        if (0 <= x2 && x2 < size && 0 <= y2 && y2 < size) decor.at(y2 >> 6, x2 >> 6) = e.v;
    }
}

// 0x0058E3C0 terrain step (0x0058EC6C..0x0058EEAF): over the component bbox grown by 200 (clipped to
// the map), copy the library class byte at the same offset from the record centre; outside 0x0F.
inline void settlement_terrain_copy(Grid<std::uint8_t>& cls, const Library& lib, int idx, std::pair<int, int> anchor, int size,
                                    int lib_max = 32767) {
    const auto& rec = lib.records.at(static_cast<std::size_t>(idx));
    const auto [xo, yo] = project(rec, anchor.first, anchor.second, size);
    long long bl = xo, br = xo, bt = yo, bb = yo;
    for (std::size_t m = 0; m < rec.comps.size(); ++m) {
        const auto [cx, cy] = rec.comps[m];
        const auto r = lib.class_rect(rec.classes[m]);
        bl = std::min(bl, static_cast<long long>(xo) + r.l + cx), br = std::max(br, static_cast<long long>(xo) + r.r + cx);
        bt = std::min(bt, static_cast<long long>(yo) + r.t + cy), bb = std::max(bb, static_cast<long long>(yo) + r.b + cy);
    }
    for (long long y = std::max(bt - 200, 0ll); y < std::min(bb + 200, static_cast<long long>(size)); y += 64) {
        const long long ly = rec.cy - yo + y;
        for (long long x = std::max(bl - 200, 0ll); x < std::min(br + 200, static_cast<long long>(size)); x += 64) {
            const long long lx = rec.cx - xo + x;
            auto& cell = cls.at(static_cast<int>(y >> 6), static_cast<int>(x >> 6));
            if (0 <= lx && lx <= lib_max && 0 <= ly && ly <= lib_max)
                cell = lib.terrain.at(py_index(ly >> 6, lib.terrain.rows), py_index(lx >> 6, lib.terrain.cols));
            else
                cell = 0x0F;
        }
    }
}

// --- influence grid (influence.py) ---------------------------------------------------------------

inline int inf_val(std::uint32_t c) {
    const int v = static_cast<int>((c >> 4) & 0x7FF);
    return (v & 0x400) ? v - 0x800 : v;
}
inline int inf_fade(std::uint32_t c) {
    const int f = static_cast<int>(c & 0xF);
    return (f & 0x8) ? f - 0x10 : f;
}
inline std::uint32_t inf_put(std::uint32_t c, int v, int f) {
    return (c & 0xFFFF8000u) | ((static_cast<std::uint32_t>(v) & 0x7FFu) << 4) | (static_cast<std::uint32_t>(f) & 0xFu);
}

inline void inf_propagate(std::vector<std::uint32_t>& g, int W, int H) {
    for (;;) {
        bool changed = false;
        for (int j = 0; j < H; ++j)
            for (int i = 0; i < W; ++i) {
                const int k = j * W + i;
                const auto c = g[static_cast<std::size_t>(k)];
                const int cand = inf_val(c) - inf_fade(c);
                if (cand <= 0) continue;
                const int f = inf_fade(c);
                const std::pair<bool, int> nbs[4] = {{i != 0, k - 1}, {i + 1 < W, k + 1}, {j != 0, k - W}, {j + 1 < H, k + W}};
                for (const auto& [ok, nk] : nbs)
                    if (ok && inf_val(g[static_cast<std::size_t>(nk)]) < cand) {
                        g[static_cast<std::size_t>(nk)] = inf_put(g[static_cast<std::size_t>(nk)], cand, f);
                        changed = true;
                    }
            }
        if (!changed) return;
    }
}

struct Influence {
    std::vector<std::uint32_t> packed;
    Grid<int> value;
};

// 0x00580350 (claims: random-map-influence-seeds-border-settlement-water-mountain-in-order,
// random-map-influence-propagation-row-major-in-place-until-stable).
inline Influence influence_grid(int size, const std::map<int, Slot>& slots, const Library& lib, const Grid<float>& a, double h,
                                const std::vector<Segment>& segments) {
    struct Inf {
        int seed, fade;
    };
    const Inf building{46, 3}, water{30, 2}, border{30, 3}, mountain{40, 3};  // CONST.INI [RandomMap]
    const int W = static_cast<int>(tdiv(size, 128)), H = W;
    std::vector<std::uint32_t> g(static_cast<std::size_t>(W) * H, 0);
    auto put = [&](long long i, long long j, const Inf& key, int extra = 0) {
        const int idx = py_index(j * W + i, static_cast<int>(g.size()));
        g[static_cast<std::size_t>(idx)] = inf_put(g[static_cast<std::size_t>(idx)], key.seed + extra, key.fade);
    };
    for (int i = 0; i < W; ++i) {
        put(i, 0, border, 2 * border.fade);
        put(i, H - 1, border, 2 * border.fade);
    }
    for (int j = 1; j < H - 1; ++j) {
        put(0, j, border);
        put(W - 1, j, border);
    }
    for (const auto& [k, s] : slots) {
        const std::pair<int, std::pair<int, int>> both[2] = {{s.village_template, s.village}, {s.town, s.primary}};
        for (const auto& [idx, xy] : both) {
            if (idx < 0) continue;
            for (const auto& [cx, cy] : lib.records.at(static_cast<std::size_t>(idx)).comps)
                put(tdiv(cx + xy.first, 128), tdiv(cy + xy.second, 128), building);
        }
    }
    const double hv = f32(h);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i)
            if (static_cast<double>(a.at(4 * j, 4 * i)) <= hv) put(i, j, water);
    for (const auto& s : segments) {
        put(tdiv(s.x0, 128), tdiv(s.y0, 128), mountain);
        put(tdiv(s.x1, 128), tdiv(s.y1, 128), mountain);
        const long long dx = s.x1 - s.x0, dy = s.y1 - s.y0;
        if (isqrt(dx * dx + dy * dy) > 128) put(tdiv(s.x0 + s.x1, 256), tdiv(s.y0 + s.y1, 256), mountain);
    }
    inf_propagate(g, W, H);
    Influence out{g, Grid<int>(H, W)};
    for (std::size_t i = 0; i < g.size(); ++i) out.value.v[i] = inf_val(g[i]);
    return out;
}

}  // namespace iberivm::rm
