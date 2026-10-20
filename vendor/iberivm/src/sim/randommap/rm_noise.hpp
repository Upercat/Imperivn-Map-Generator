// Procedural value-noise field 0x00653640..0x00654020 and the field helpers of 0x00581D20
// (reference: tools/randommap/noise.py and field_a.py). The native code runs the x87 at 53-bit
// precision; the reference emulates it with float64 and explicit float32 stores, reproduced here
// operation by operation (double for the PC53 intermediates, float where the native stores).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include "rm_common.hpp"

namespace iberivm::rm {

using Amps = std::array<float, 13>;

// 0x0065369B..0x006536B7
inline int log2_field(int n) {
    int e = 0;
    for (int v = n >> 1; v; v >>= 1) ++e;
    return e;
}

// 0x006536D0: amp[start+k] = f32(run), run *= f32(p) (PC53)
inline Amps set_amps(int start, int count, double p) {
    Amps amp{};
    double run = 1.0;
    const double pf = static_cast<double>(f32(p));
    for (int k = 0; k < count; ++k) {
        amp[static_cast<std::size_t>(start + k)] = f32(run);
        run = run * pf;
    }
    return amp;
}

// Private LCG of the noise generator: state = state * 0x3D73 + 0xC0AE5, value = state & 0xFFF.
inline std::uint32_t noise_lcg(std::uint32_t& state) {
    state = state * 0x3D73u + 0xC0AE5u;
    return state & 0xFFFu;
}

// Octave sum with Catmull-Rom interpolation of each lattice (noise.generate). `user` replaces the
// random lattice of an octave by amp * user (0x005CE580 template-field upsample).
inline Grid<float> noise_generate(int n, const Amps& amp, std::uint32_t seed, const std::map<int, const Grid<float>*>* user = nullptr) {
    constexpr double K = 1.0 / 4096.0;  // 0x007DA24C = 0x39800000
    const int L = log2_field(n);
    Grid<float> data(n, n, 0.0f);
    std::uint32_t state = seed;
    for (int o = 1; o < L; ++o) {
        const int M = 1 << o, s = n >> o;
        const double a = static_cast<double>(amp[static_cast<std::size_t>(o)]);
        std::vector<float> lat(static_cast<std::size_t>(M) * M);
        const Grid<float>* u = nullptr;
        if (user) {
            const auto it = user->find(o);
            if (it != user->end()) u = it->second;
        }
        if (u) {
            for (std::size_t i = 0; i < lat.size(); ++i) lat[i] = f32(a * static_cast<double>(u->v[i]));
        } else {
            for (auto& x : lat) {
                const double r = static_cast<double>(noise_lcg(state));
                x = f32(((1.0 * r) * a) * K);
            }
        }
        std::vector<double> t(static_cast<std::size_t>(s));
        for (int j = 0; j < s; ++j) t[static_cast<std::size_t>(j)] = static_cast<double>(j) / s;
        auto clampi = [M](int v) { return v < 0 ? 0 : (v > M - 1 ? M - 1 : v); };
        for (int k = 0; k < M; ++k)
            for (int i = 0; i < M; ++i) {
                float patch[4][4];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) patch[r][c] = lat[static_cast<std::size_t>(clampi(k - 1 + r)) * M + clampi(i - 1 + c)];
                double a0[4], a1[4], a2[4], a3[4];
                for (int c = 0; c < 4; ++c) {
                    const double p0 = patch[0][c], p1 = patch[1][c], p2 = patch[2][c], p3 = patch[3][c];
                    const float f0 = f32(((p3 - p2) - p0) + p1);
                    a0[c] = f0;
                    a1[c] = f32((p0 - p1) - static_cast<double>(f0));
                    a2[c] = f32(p2 - p0);
                    a3[c] = p1;
                }
                for (int j = 0; j < s; ++j) {
                    const double ty = t[static_cast<std::size_t>(j)];
                    double v[4];
                    for (int c = 0; c < 4; ++c) v[c] = ((a0[c] * ty + a1[c]) * ty + a2[c]) * ty + a3[c];
                    const double v1f = f32(v[1]);
                    const double b0 = f32(((v[3] - v[2]) - v[0]) + v1f);
                    const double b1 = f32((v[0] - v1f) - b0);
                    const double b2 = v[2] - v[0];
                    float* row = &data.v[static_cast<std::size_t>(k * s + j) * n + static_cast<std::size_t>(i) * s];
                    for (int x = 0; x < s; ++x) {
                        const double tx = t[static_cast<std::size_t>(x)];
                        const double poly = ((b0 * tx + b1) * tx + b2) * tx;
                        row[x] = f32((poly + static_cast<double>(row[x])) + v1f);
                    }
                }
            }
    }
    if (amp[static_cast<std::size_t>(L)] != 0.0f) {  // 0x00653F37..0x00653FC2
        const double a = static_cast<double>(amp[static_cast<std::size_t>(L)]);
        for (auto& x : data.v) {
            const double r = static_cast<double>(noise_lcg(state));
            x = f32((r * a) * K + static_cast<double>(x));
        }
    }
    return data;
}

// 0x00653730: (v - min) * f32(1 / (max - min)) at PC53.
inline Grid<float> noise_normalise(const Grid<float>& g) {
    float lo = g.v[0], hi = g.v[0];
    for (float x : g.v) {
        if (x < lo) lo = x;
        if (x > hi) hi = x;
    }
    const double scale = f32(1.0 / (static_cast<double>(hi) - static_cast<double>(lo)));
    Grid<float> out(g.rows, g.cols);
    for (std::size_t i = 0; i < g.v.size(); ++i) out.v[i] = f32((static_cast<double>(g.v[i]) - lo) * scale);
    return out;
}

// 0x005820A6..0x005820DC: trunc((v + 0.1f) * 5.0f) * 0.2f, capped at 1.0.
inline Grid<float> quantise(const Grid<float>& g) {
    const double c01 = f32(0.1), c5 = f32(5.0), c02 = f32(0.2);
    Grid<float> out(g.rows, g.cols);
    for (std::size_t i = 0; i < g.v.size(); ++i) {
        const double v = (static_cast<double>(g.v[i]) + c01) * c5;
        const double r = std::trunc(v) * c02;
        out.v[i] = r > 1.0 ? 1.0f : f32(r);
    }
    return out;
}

// Native in-place (2R+1)^2 mean, x87 double accumulation, dx outer / dy inner, earlier cells already
// replaced (field_a.box_inplace).
inline void box_inplace(Grid<float>& g, int R) {
    const int n = g.rows;
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            double s = 0.0;
            int cnt = 0;
            for (int dx = -R; dx <= R; ++dx) {
                const int cc = c + dx;
                if (cc < 0 || cc >= n) continue;
                for (int dy = -R; dy <= R; ++dy) {
                    const int rr = r + dy;
                    if (rr >= 0 && rr < n) {
                        s += static_cast<double>(g.v[static_cast<std::size_t>(rr) * n + cc]);
                        ++cnt;
                    }
                }
            }
            g.v[static_cast<std::size_t>(r) * n + c] = f32(s / cnt);
        }
}

}  // namespace iberivm::rm
