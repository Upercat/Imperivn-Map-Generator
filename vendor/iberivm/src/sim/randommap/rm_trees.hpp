// PlaceTrees (reference: exact.weighted_pick and exact.place_trees).
#pragma once
#include <cstdint>
#include <optional>
#include <vector>
#include "randommap_rules.hpp"
#include "rm_common.hpp"

namespace iberivm::rm {

// 0x0057B7D0 (claim:random-map-tree-weighted-pick-cumulative-walk).
inline int weighted_pick(DrawLog& rng, const std::vector<rmdata::TreeTable::Entry>& entries, int first, int count) {
    if (count == 0) return first;
    int total = 0;
    for (int k = first; k < first + count; ++k) total += entries[static_cast<std::size_t>(k)].w;
    int r = rng(0, total - 1);
    for (int k = first; k < first + count; ++k) {
        if (r < entries[static_cast<std::size_t>(k)].w) return k;
        r -= entries[static_cast<std::size_t>(k)].w;
    }
    return first + count - 1;
}

// 0x0057C060 (claims: random-map-placetrees-draw-gated-by-influence-and-threshold,
// random-map-placetrees-cluster-copy-within-r1-regroup-within-r2,
// random-map-tree-decor-type-uniform-then-random-high-byte).
inline void place_trees(DrawLog& rng, const Grid<float>& field, const Grid<int>& inf, const Grid<std::uint8_t>& zone,
                        const std::vector<rmdata::TreeTable>& tables, Grid<std::uint16_t>& decor, double thres = 0.15,
                        double dens = 0.10, int infmax = 29, int r1 = 3, int r2 = 10) {
    const int n = field.rows;
    const double th = f32(thres), dn = f32(dens), eps = f32(1e-4);
    Grid<long long> mask(n, n, 0);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int v = inf.at((j * 64) >> 7, (i * 64) >> 7);
            if (v > infmax) continue;
            if (static_cast<double>(field.at(j, i)) < th) continue;
            const double p = f32((infmax - v) * dn / (2 * infmax) + dn * 0.5);
            const int r = rng(0, 9999);
            if (r * eps <= p) mask.at(j, i) = 0xFF;
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            if (mask.at(j, i) != 0xFF) continue;
            const auto& tb = tables[static_cast<std::size_t>(zone.at((j * 64) >> 7, (i * 64) >> 7) & 3)];
            std::optional<std::pair<int, long long>> found;
            for (int r = 1; r <= r2 && !found; ++r) {
                for (int k = 0; k <= std::min(r, j) && !found; ++k) {
                    const int cand[2][3] = {{j - k, i - r + k, i - r + k >= 0}, {j - k, i + r - k, k != 0 && i + r - k < n}};
                    for (const auto& c : cand)
                        if (c[2] && mask.at(c[0], c[1])) {
                            found = std::make_pair(r, mask.at(c[0], c[1]));
                            break;
                        }
                }
            }
            if (found && found->first <= r1) {
                mask.at(j, i) = found->second;
            } else if (found) {
                const long long g = (found->second >> 4) - 1;
                const auto& ge = tb.G.at(static_cast<std::size_t>(g));
                const int v = weighted_pick(rng, tb.S, ge.first, ge.n);
                mask.at(j, i) = (found->second & 0xF0) | v;
            } else {
                const int g = weighted_pick(rng, tb.G, 0, static_cast<int>(tb.G.size()));
                const auto& ge = tb.G.at(static_cast<std::size_t>(g));
                const int v = weighted_pick(rng, tb.S, ge.first, ge.n);
                mask.at(j, i) = (static_cast<long long>(g + 1) << 4) | v;
            }
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const long long b = mask.at(j, i);
            if (!b) continue;
            const auto& tb = tables[static_cast<std::size_t>(zone.at((j * 64) >> 7, (i * 64) >> 7) & 3)];
            int t;
            if (!tb.G.empty()) {
                const auto& s = tb.S[static_cast<std::size_t>((b & 0xF) % static_cast<long long>(tb.S.size()))];
                t = tb.T[static_cast<std::size_t>(s.first + rng(0, s.n - 1))];
            } else
                t = 0x0D;
            const int r8 = rng(0, 255);
            decor.at(j, i) = static_cast<std::uint16_t>((r8 << 8) | (t & 0xFF));
        }
}

}  // namespace iberivm::rm
