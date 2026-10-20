// Template phase of the random-map generator (reference: tools/randommap/template_raster.py,
// strongholds.py and exact.template_phase): distortion fields, bitmap raster with palette
// classification, symmetry, marker flood fills and labels, mountain segments, and the native
// stronghold assignment search 0x005D1090.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <vector>
#include "rm_common.hpp"
#include "rm_noise.hpp"

namespace iberivm::rm {

constexpr int TN = 512;  // template raster side

// 0x005CE280 (PC53): normalised noise - 0.5, tapered over w = f32(512 * amp) cells at both ends of
// the axis (columns for X, rows for Y).
inline Grid<float> distortion(std::uint32_t seed, double amp, bool axis_x) {
    auto v = noise_normalise(noise_generate(TN, set_amps(2, 4, 0.5), seed));
    const double half = f32(0.5);
    for (auto& x : v.v) x = f32(static_cast<double>(x) - half);
    const double nf = static_cast<double>(f32(TN));
    const double w = f32(nf * static_cast<double>(f32(amp)));
    const double inv = w != 0.0 ? static_cast<double>(f32(1.0 / w)) : std::numeric_limits<double>::infinity();
    for (int k = 0; k < TN; ++k) {
        for (int m = 0; m < TN; ++m) {
            float& cell = axis_x ? v.at(m, k) : v.at(k, m);
            if (k < w) cell = f32(inv * k * static_cast<double>(cell));
            if (k > nf - w) cell = f32((TN - k) * inv * static_cast<double>(cell));
        }
    }
    return v;
}

// 0x0063C890: first strict Chebyshev minimum over the palette.
inline std::uint8_t palette_class(const std::array<int, 3>& rgb, const std::vector<std::array<int, 3>>& pal) {
    int best = 0x7FFF;
    std::uint8_t idx = 0;
    for (std::size_t i = 0; i < pal.size(); ++i) {
        int d = 0;
        for (int c = 0; c < 3; ++c) d = std::max(d, std::abs(rgb[static_cast<std::size_t>(c)] - pal[i][static_cast<std::size_t>(c)]));
        if (d < best) {
            best = d;
            idx = static_cast<std::uint8_t>(i);
        }
    }
    return idx;
}

// 0x005D0AF0: sample the bitmap at the distorted position (x rounded through float, y kept at PC53).
inline Grid<std::uint8_t> raster(const std::vector<std::array<int, 3>>& rgb, const Grid<float>& X, const Grid<float>& Y, double amp,
                                 const std::vector<std::array<int, 3>>& pal) {
    const double a = static_cast<double>(f32(amp));
    Grid<std::uint8_t> out(TN, TN);
    for (int yy = 0; yy < TN; ++yy)
        for (int xx = 0; xx < TN; ++xx) {
            const double sx = static_cast<double>(f32(static_cast<double>(X.at(yy, xx)) * a * 512.0 + xx));
            const double sy = static_cast<double>(Y.at(yy, xx)) * a * 512.0 + yy;
            const int tx = static_cast<int>(std::trunc(std::clamp(sx, 0.0, 511.0)));
            const int ty = static_cast<int>(std::trunc(std::clamp(sy, 0.0, 511.0)));
            out.at(yy, xx) = palette_class(rgb[static_cast<std::size_t>(ty) * TN + tx], pal);
        }
    return out;
}

// 0x005CE0F0
inline Grid<std::uint8_t> symmetry(const Grid<std::uint8_t>& g, int r) {
    Grid<std::uint8_t> out(TN, TN);
    const int rot = r & 6;
    for (int i = 0; i < TN; ++i)
        for (int j = 0; j < TN; ++j) {
            int row = i, col = j;
            if (rot == 2) row = j, col = 511 - i;
            else if (rot == 4) row = 511 - i, col = 511 - j;
            else if (rot == 6) row = 511 - j, col = i;
            if (r & 1) col = 511 - col;
            out.at(i, j) = g.at(row, col);
        }
    return out;
}

// 0x005CFAC0: 4-neighbour breadth-first fill; returns the floored centroid.
inline std::pair<int, int> flood(Grid<std::uint8_t>& g, int x, int y, int match, int fresh) {
    g.at(y, x) = static_cast<std::uint8_t>(fresh);
    std::deque<std::pair<int, int>> q{{x, y}};
    long long sx = 0, sy = 0, n = 0;
    while (!q.empty()) {
        const auto [cx, cy] = q.front();
        q.pop_front();
        sx += cx, sy += cy, ++n;
        const int nb[4][3] = {{cx, cy - 1, cy > 0}, {cx - 1, cy, cx > 0}, {cx + 1, cy, cx < 511}, {cx, cy + 1, cy < 511}};
        for (const auto& e : nb)
            if (e[2] && g.at(e[1], e[0]) == match) {
                g.at(e[1], e[0]) = static_cast<std::uint8_t>(fresh);
                q.emplace_back(e[0], e[1]);
            }
    }
    return {static_cast<int>(sx / n), static_cast<int>(sy / n)};
}

using Markers = std::vector<std::vector<std::pair<int, int>>>;  // per palette class: centroids (x, y)

// 0x005CFCE0: centroids of every non-sea, non-land region inside the margin 0x96000 / size, then
// labels of the land components (2, 3, ..., wrapping to 2 after 255).
inline std::pair<Markers, Grid<std::uint8_t>> markers(const Grid<std::uint8_t>& sem, int size) {
    auto g = sem;
    const int m = 0x96000 / size;
    Markers vec(256);
    for (int y = 0; y < TN; ++y)
        for (int x = 0; x < TN; ++x) {
            const int c = g.at(y, x);
            if (c == 0 || c == 1) continue;
            const auto [cx, cy] = flood(g, x, y, c, 1);
            if (m <= cx && cx <= 511 - m && m <= cy && cy <= 511 - m) vec[static_cast<std::size_t>(c)].emplace_back(cx, cy);
        }
    int lab = 2;
    for (int y = 0; y < TN; ++y)
        for (int x = 0; x < TN; ++x)
            if (g.at(y, x) == 1) {
                flood(g, x, y, 1, lab);
                lab = (lab + 1) & 0xFF;
                if (!lab) lab = 2;
            }
    return {vec, g};
}

struct Segment {
    int x0, y0, x1, y1;
};

// 0x005D0830 (x87 PC53 distances): chain each mountain marker to its nearest unvisited marker of the
// same land label while the distance stays below 32.
inline std::vector<Segment> segments(const std::vector<std::pair<int, int>>& mnt, const Grid<std::uint8_t>& g) {
    std::vector<int> vis(mnt.size(), 0);
    std::vector<Segment> segs;
    for (;;) {
        const auto it = std::find(vis.begin(), vis.end(), 0);
        if (it == vis.end()) break;
        int cur = static_cast<int>(it - vis.begin());
        for (;;) {
            vis[static_cast<std::size_t>(cur)] = 1;
            double best = f32(262144.0);
            int bi = -1;
            const auto [cx, cy] = mnt[static_cast<std::size_t>(cur)];
            for (std::size_t j = 0; j < mnt.size(); ++j) {
                const auto [xj, yj] = mnt[j];
                if (vis[j] || g.at(yj, xj) != g.at(cy, cx)) continue;
                const double d = std::sqrt(static_cast<double>((cy - yj) * (cy - yj) + (cx - xj) * (cx - xj)));
                if (d < best) best = f32(d), bi = static_cast<int>(j);
            }
            if (!(best < 32.0)) break;
            const auto [bx, by] = mnt[static_cast<std::size_t>(bi)];
            if (isqrt(static_cast<long long>(bx - cx) * (bx - cx) + static_cast<long long>(by - cy) * (by - cy)) > 0)
                segs.push_back({cx, cy, bx, by});
            cur = bi;
        }
    }
    return segs;
}

// 0x005D18B0 (claim:random-map-template-normalizes-settings-team-bytes-into-team-vector).
inline std::vector<int> team_vector(const std::vector<int>& teams) {
    bool used[16] = {};
    for (int v : teams)
        if (v > 0) used[v] = true;
    int f = 0;
    std::vector<int> out;
    for (int v : teams) {
        if (v == 0) {
            out.push_back(f);
            ++f;
            while (f < 16 && used[f]) ++f;
        } else
            out.push_back(v);
    }
    while (!out.empty() && out.back() == -1) out.pop_back();
    return out;
}

// Native stronghold assignment 0x005D1090 with score 0x005CFEF0 and adjacency 0x005CE4E0
// (strongholds.Native).
class StrongholdSearch {
public:
    StrongholdSearch(std::vector<std::pair<int, int>> mk, const Grid<std::uint8_t>& labels, std::vector<int> team, int size)
        : mk_(std::move(mk)), labels_(labels), team_(std::move(team)), size_(size), n_(static_cast<int>(team_.size())) {}

    std::optional<std::vector<int>> search(DrawLog& rng) {
        const int n = n_, m = static_cast<int>(mk_.size());
        auto v1 = gen_unique(rng);
        int retries = 500, fresh = 128, stall = 0, i = 0;
        const int bound = 3 * n * n;
        while (i < bound) {
            const auto [cur, worst] = score(v1);
            std::vector<std::vector<int>> v2;
            if (worst != -1) {
                for (int e = 0; e < n; ++e) {
                    if (team_[e] == team_[worst] || team_[e] == -1) continue;
                    auto p = v1;
                    std::swap(p[e], p[worst]);
                    v2.push_back(p);
                }
                for (int k = 0; k < 24; ++k) {
                    int tries = 0, r = 0;
                    bool none = false;
                    for (;;) {
                        r = rng(0, m - 1);
                        if (std::find(v1.begin(), v1.end(), r) == v1.end()) break;
                        if (++tries >= 100) {
                            none = true;
                            break;
                        }
                    }
                    if (none) break;
                    auto p = v1;
                    p[worst] = r;
                    v2.push_back(p);
                }
            }
            for (int k = 0; k < fresh; ++k) v2.push_back(gen_unique(rng));
            std::vector<float> v3;
            for (const auto& p : v2) v3.push_back(score(p).first);
            std::size_t best = 0;
            for (std::size_t e = 1; e < v3.size(); ++e)
                if (v3[e] < v3[best]) best = e;
            if (!(v3[best] < plus_inf())) {
                if (retries == 0) return std::nullopt;
                --i, --retries, fresh += 2;
            } else if (v3[best] < cur) {
                v1 = v2[best], stall = 0;
            } else if (++stall == 25)
                break;
            ++i;
        }
        if (retries == 0) return std::nullopt;
        const long long lim = tdiv(0x1B5800, size_);
        for (int j = 0; j < n; ++j) {
            if (team_[j] == -1) continue;
            for (int k = 0; k < n; ++k) {
                if (team_[k] == -1 || k == j) continue;
                const auto [xj, yj] = mk_[static_cast<std::size_t>(v1[j])];
                const auto [xk, yk] = mk_[static_cast<std::size_t>(v1[k])];
                if (isqrt(static_cast<long long>(xk - xj) * (xk - xj) + static_cast<long long>(yk - yj) * (yk - yj)) < lim)
                    return std::nullopt;
            }
        }
        return v1;
    }

private:
    std::vector<std::pair<int, int>> mk_;
    const Grid<std::uint8_t>& labels_;
    std::vector<int> team_;
    int size_, n_;

    static float plus_inf() {
        const std::uint32_t bits = 0x5368D4A5u;
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    double adj(int ia, int ib) const {
        const auto [xa, ya] = mk_[static_cast<std::size_t>(ia)];
        const auto [xb, yb] = mk_[static_cast<std::size_t>(ib)];
        const double dx = xa - xb, dy = ya - yb;
        double d = std::sqrt(dx * dx + dy * dy);  // math.hypot of small integers (exact squares)
        if (labels_.at(ya, xa) != labels_.at(yb, xb) && size_ >= 0x4000) d = d + 384.0;
        return d;
    }

    std::pair<float, int> score(const std::vector<int>& asg) const {
        const int n = n_;
        const long long q = tdiv(-0x80000, size_);
        const long long lo = -q, hi = q + 0x1FF;
        for (int c = 0; c < n; ++c) {
            if (team_[c] == -1) continue;
            const auto [x, y] = mk_[static_cast<std::size_t>(asg[c])];
            if (x < lo || x > hi || y < lo || y > hi) return {plus_inf(), c};
        }
        std::vector<float> M(static_cast<std::size_t>(n) * n, 0.0f);
        std::vector<char> have(M.size(), 0);
        float min_all = plus_inf();
        for (int a = 0; a < n; ++a) {
            if (team_[a] == -1) continue;
            for (int b = 0; b < n; ++b) {
                if (team_[b] == -1 || a == b) continue;
                double val;
                if (!have[static_cast<std::size_t>(b * n + a)]) {
                    const double d = adj(asg[a], asg[b]);
                    M[static_cast<std::size_t>(a * n + b)] = M[static_cast<std::size_t>(b * n + a)] = f32(d);
                    have[static_cast<std::size_t>(a * n + b)] = have[static_cast<std::size_t>(b * n + a)] = 1;
                    val = d;
                } else
                    val = M[static_cast<std::size_t>(b * n + a)];
                if (static_cast<double>(min_all) > val) min_all = f32(val);
            }
        }
        const long long lim = tdiv(0x1B5800, size_);
        if (!(static_cast<double>(lim) < static_cast<double>(min_all))) return {plus_inf(), -1};
        std::vector<float> L1, L2, D(static_cast<std::size_t>(n), 0.0f);  // never cleared between players
        for (int a = 0; a < n; ++a) {
            if (team_[a] == -1) continue;
            float s1 = 0.0f, s2 = 0.0f;
            for (int b = 0; b < n; ++b)
                if (team_[b] != -1 && b != a && team_[b] == team_[a]) {
                    L1.push_back(M[static_cast<std::size_t>(a * n + b)]);
                    s1 = f32(static_cast<double>(M[static_cast<std::size_t>(a * n + b)]) + static_cast<double>(s1));
                }
            for (int b = 0; b < n; ++b)
                if (team_[b] != -1 && team_[b] != team_[a]) {
                    L2.push_back(M[static_cast<std::size_t>(a * n + b)]);
                    s2 = f32(static_cast<double>(M[static_cast<std::size_t>(a * n + b)]) + static_cast<double>(s2));
                }
            float mally = plus_inf(), menemy = plus_inf();
            for (float v : L1)
                if (v < mally) mally = v;
            for (float v : L2)
                if (v < menemy) menemy = v;
            if (L1.empty() && L2.empty()) return {0.0f, 0};
            if (L1.empty()) {
                D[static_cast<std::size_t>(a)] = f32(64.0 / static_cast<double>(menemy));
                continue;
            }
            if (L2.empty()) {
                D[static_cast<std::size_t>(a)] = 0.0f;
                continue;
            }
            const double ratio = (static_cast<double>(L2.size()) * static_cast<double>(s1)) /
                                 (static_cast<double>(L1.size()) * static_cast<double>(s2));
            double v = std::sqrt(ratio) * ((1.0 / static_cast<double>(min_all)) * 512.0);
            v = v * ((1.0 / static_cast<double>(mally)) * 512.0);
            v = v * (512.0 / static_cast<double>(menemy));
            float& da = D[static_cast<std::size_t>(a)];
            da = f32(v);
            if (static_cast<double>(min_all) <= static_cast<double>(lim))
                da = f32(static_cast<double>(lim) * (1.0 / static_cast<double>(min_all)) * static_cast<double>(da));
            if (static_cast<double>(mally) <= static_cast<double>(lim))
                da = f32(static_cast<double>(lim) * (1.0 / static_cast<double>(mally)) * static_cast<double>(da));
            if (static_cast<double>(2 * lim) >= static_cast<double>(mally)) da = f32(static_cast<double>(da) * 2.0);
        }
        int best = 0;
        for (int e = 1; e < n; ++e)
            if (D[static_cast<std::size_t>(best)] < D[static_cast<std::size_t>(e)]) best = e;
        return {D[static_cast<std::size_t>(best)], best};
    }

    // 0x005D06F0
    std::vector<int> gen_unique(DrawLog& rng) const {
        const int m = static_cast<int>(mk_.size());
        std::vector<int> out;
        for (int j = 0; j < n_; ++j) {
            if (team_[j] == -1) {
                out.push_back(-1);
                continue;
            }
            int r = rng(0, m - 1);
            auto taken = [&](int v) {
                for (int c = 0; c < j; ++c)
                    if (team_[c] != -1 && out[static_cast<std::size_t>(c)] == v) return true;
                return false;
            };
            while (taken(r)) r = rng(0, m - 1);
            out.push_back(r);
        }
        return out;
    }
};

}  // namespace iberivm::rm
