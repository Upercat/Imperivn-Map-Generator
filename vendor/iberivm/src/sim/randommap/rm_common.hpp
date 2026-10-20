// Shared pieces of the random-map generator port (tools/randommap is the byte-exact reference and
// verifier): grids, C-style integer division, the primary EngineRandom with its draw log, and the
// stage sink through which every generation stage reports its output.
#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>
#include "game_rng.hpp"

namespace iberivm::rm {

template <class T>
struct Grid {
    int rows = 0, cols = 0;
    std::vector<T> v;
    Grid() = default;
    Grid(int r, int c, T fill = T{}) : rows(r), cols(c), v(static_cast<std::size_t>(r) * c, fill) {}
    T& at(int r, int c) { return v[static_cast<std::size_t>(r) * cols + c]; }
    const T& at(int r, int c) const { return v[static_cast<std::size_t>(r) * cols + c]; }
};

// C integer division and remainder (truncate toward zero), as tdiv/crem in exact.py.
inline long long tdiv(long long a, long long b) {
    const long long q = (a < 0 ? -a : a) / (b < 0 ? -b : b);
    return (a >= 0) == (b > 0) ? q : -q;
}
inline long long crem(long long a, long long b) { return a - tdiv(a, b) * b; }

// Integer square root (math.isqrt) of a non-negative value.
inline long long isqrt(long long v) {
    if (v <= 0) return 0;
    long long r = static_cast<long long>(std::sqrt(static_cast<double>(v)));
    while (r * r > v) --r;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}

inline float f32(double v) { return static_cast<float>(v); }

// Primary EngineRandom at game+0x12A0 (claim:engine-random-next-inclusive-range-draw-semantics) with
// the advancing-draw counter of the captures and a log of every advancing draw.
struct DrawLog {
    struct Draw {
        std::int32_t lo, hi, value;
    };
    GameRng rng;
    std::uint32_t draws = 0;
    std::vector<Draw> log;
    bool record_history = true;
    explicit DrawLog(std::uint32_t state) : rng(state) {}
    std::int32_t operator()(std::int32_t a, std::int32_t b) {
        const auto v = rng.next(a, b);
        if (a != b) {
            ++draws;
            if (record_history) log.push_back({a, b, v});
        }
        return v;
    }
};

// Receives every stage output: name, element type extension (f32, u8, u16, i32, u32, bin, xml) and
// the raw little-endian bytes. The app writes them to files; sim never touches the file system here.
using StageSink = std::function<void(const std::string& name, const std::string& ext, const std::vector<std::uint8_t>& bytes,
                                     std::uint32_t draws)>;

template <class T>
std::vector<std::uint8_t> raw_bytes(const std::vector<T>& v) {
    std::vector<std::uint8_t> out(v.size() * sizeof(T));
    if (!v.empty()) std::memcpy(out.data(), v.data(), out.size());
    return out;
}

}  // namespace iberivm::rm
