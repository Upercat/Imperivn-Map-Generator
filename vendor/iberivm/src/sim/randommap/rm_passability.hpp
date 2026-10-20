// Passability rebuild after generation 0x00547700 (reference: tools/randommap/passability.py):
// Map_RasterizeTerrainPassability 0x00547090, CVXMap::RasterizeEntityCollisionInRect 0x00546E00,
// Map_RasterizeEntityCollisionFootprint 0x00546A80, CVXEntity::ComputeFootprintBounds 0x005FDF70,
// ScreenToWorld raycast 0x006243B0, Terrain_SampleHeightAtWorldPoint 0x0053EAA0 / 0x0053DC00,
// Map_BlockPassabilityBorders 0x00544BD0. Bit = 1 blocked, one bit per 16-unit cell.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "game_fs.hpp"
#include "randommap_data.hpp"
#include "rm_common.hpp"
#include "rm_fields.hpp"
#include "xml_lite.hpp"

namespace iberivm::rm {

inline long long s32(long long v) {
    v &= 0xFFFFFFFFll;
    return (v & 0x80000000ll) ? v - (1ll << 32) : v;
}

// Python >> on a (possibly negative) integer: floor division by a power of two.
inline long long asr(long long v, int s) { return v >= 0 ? v >> s : -((-v + (1ll << s) - 1) >> s); }

struct EntityFootprint {
    Grid<std::uint8_t> grid;  // 16-unit cells, rows = h >> 4
    long long minx = 0, miny = 0, maxx = 0, maxy = 0;
};

// Static passability inputs: DATA/TERRAINS.XML layers and entity pass grids (+0x298, bounds +0x2A4..+0x2B0).
class PassData {
public:
    explicit PassData(const GameFS& fs) : fs_(fs) {
        const auto t = fs.read_text("DATA/TERRAINS.XML");
        for (const auto& body : xml::self_closing_bodies(t, "layer")) {
            const auto a = xml::regex_attrs(body);
            const auto* p = xml::regex_get(a, "passable");
            const auto* w = xml::regex_get(a, "passable_water");
            terrain_[static_cast<int>(rmdata::py_int(*xml::regex_get(a, "z")))] = {static_cast<int>(rmdata::py_int(p ? *p : "1")),
                                                                                    static_cast<int>(rmdata::py_int(w ? *w : "0"))};
        }
    }
    std::pair<int, int> terrain(int z) const {
        const auto it = terrain_.find(z);
        if (it == terrain_.end()) throw std::runtime_error("TERRAINS.XML has no layer z=" + std::to_string(z));
        return it->second;
    }

    const EntityFootprint* footprint(const std::optional<std::string>& entity_path) {
        if (!entity_path || entity_path->empty()) return nullptr;
        const auto key = GameFS::key(*entity_path);
        if (const auto it = cache_.find(key); it != cache_.end()) return it->second ? &*it->second : nullptr;
        auto& slot = cache_[key];
        try {
            if (!fs_.exists(key)) return nullptr;
            const auto t = fs_.read_text(key);
            const std::string tag = "pass_file=\"";
            const auto b = t.find(tag);
            if (b == std::string::npos) return nullptr;
            const auto e = t.find('"', b + tag.size());
            if (e == std::string::npos || e == b + tag.size()) return nullptr;
            const auto slash = key.rfind('/');
            const std::string parent = slash == std::string::npos ? "" : key.substr(0, slash);
            const auto pf = GameFS::key(parent + "/" + t.substr(b + tag.size(), e - b - tag.size()));
            if (!fs_.exists(pf)) return nullptr;
            const auto raw = fs_.read(pf);
            const auto cell = rmdata::le32(raw, 4), bits = rmdata::le32(raw, 8), w = rmdata::le32(raw, 12), h = rmdata::le32(raw, 16);
            if (cell != 16 || bits != 1) throw std::runtime_error("unexpected pass grid: " + pf);
            const auto stride = w >> 9;
            EntityFootprint fp;
            fp.grid = Grid<std::uint8_t>(static_cast<int>(h >> 4), static_cast<int>(stride * 32), 0);
            long long xmin = -1, xmax = -1, ymin = -1, ymax = -1;
            for (int r = 0; r < fp.grid.rows; ++r)
                for (std::uint32_t wd = 0; wd < stride; ++wd) {
                    const auto word = rmdata::le32(raw, 20 + (static_cast<std::size_t>(r) * stride + wd) * 4);
                    for (int bit = 0; bit < 32; ++bit)
                        if ((word >> bit) & 1u) {
                            const int c = static_cast<int>(wd * 32 + static_cast<std::uint32_t>(bit));
                            fp.grid.at(r, c) = 1;
                            if (xmin < 0 || c < xmin) xmin = c;
                            if (c > xmax) xmax = c;
                            if (ymin < 0) ymin = r;
                            ymax = r;
                        }
                }
            if (xmin < 0) fp.minx = 0x7FF - 0x10, fp.miny = 0x7FF - 0x10, fp.maxx = 0x10, fp.maxy = 0x10;
            else fp.minx = xmin * 16 - 16, fp.miny = ymin * 16 - 16, fp.maxx = xmax * 16 + 16, fp.maxy = ymax * 16 + 16;
            slot = std::move(fp);
            return &*slot;
        } catch (...) {
            cache_.erase(key);
            throw;
        }
    }

private:
    const GameFS& fs_;
    std::map<int, std::pair<int, int>> terrain_;
    std::map<std::string, std::optional<EntityFootprint>> cache_;
};

class Passability {
public:
    Passability(int size, const Grid<std::uint8_t>& heights, const Grid<std::uint8_t>& cls,
                Grid<std::uint8_t> workspace = {})
        : W_(size), H_(size), R_(size - 1), B_(size - 1), h_(heights), cls_(cls), bits(std::move(workspace)) {
        bits.rows = bits.cols = size >> 4;
        bits.v.assign(static_cast<std::size_t>(bits.rows) * bits.cols, 0);
    }

    Grid<std::uint8_t> bits;

    // 0x0053EAA0 / 0x0053DC00
    long long height(long long x, long long y) const {
        if (!(0 <= x && x <= R_ && 0 <= y && y <= B_)) return 0;
        const int last = h_.rows - 1;
        const int cx = static_cast<int>(x >> 5), cy = static_cast<int>(y >> 5);
        auto H = [&](int r, int c) { return static_cast<long long>(h_.at(py_index(r, h_.rows), py_index(c, h_.cols))); };
        if ((x & 31) == 0 && (y & 31) == 0) return H(cy, cx);
        long long a, b, c, d;
        if ((cx & 3) == 3) {
            a = H(cy, cx);
            b = cx < last ? H(cy, cx + 1) : a;
            const int ry = cy < last ? cy + 1 : cy;
            c = H(ry, cx);
            d = cx < last ? H(ry, cx + 1) : c;
        } else {
            a = H(cy, cx), b = H(cy, cx + 1);
            if (cy < last) c = H(cy + 1, cx), d = H(cy + 1, cx + 1);
            else c = a, d = b;
        }
        const long long u = x - (x & ~31ll), v = y - (y & ~31ll);
        const long long top = tdiv((b - a) * u, 32) + a;
        const long long bot = tdiv((d - c) * u, 32) + c;
        return tdiv((bot - top) * v, 32) + top;
    }

    // 0x006243B0
    std::pair<long long, long long> raycast(long long sx, long long sy) const {
        long long ly = (asr(sy * 0x16A, 8) & ~31ll) & 0xFFFFFFFFll;
        long long cur = asr(s32(ly * 0xB5), 8) - height(sx, s32(ly));
        long long prev = -10000;
        while (cur <= sy) {
            prev = cur;
            ly = (ly + 0x20) & 0xFFFFFFFFll;
            cur = asr(s32((ly * 0xB5) & 0xFFFFFFFFll), 8) - height(sx, s32(ly));
        }
        ly = (ly - 0x20) & 0xFFFFFFFFll;
        if (prev == -10000) prev = asr(s32((ly * 0xB5) & 0xFFFFFFFFll), 8) - height(sx, s32(ly));
        return {sx, s32((ly + tdiv((sy - prev) * 0x20, cur - prev)) & 0xFFFFFFFFll)};
    }

    void set(long long x, long long y) {
        bits.at(py_index((y & 0xFFFFFFFFll) >> 4, bits.rows), py_index((x & 0xFFFFFFFFll) >> 4, bits.cols)) = 1;
    }

    int class_at(long long x, long long y) const { return cls_.at(py_index(asr(y, 6), cls_.rows), py_index(asr(x, 6), cls_.cols)); }

    // 0x00547090
    void terrain(const PassData& pd) {
        for (long long x = 0; x <= R_ && x + 0x20 < W_; x += 16)
            for (long long y = 0; y <= B_ && y + 0x20 < H_; y += 16) {
                const auto [passable, water] = pd.terrain(class_at(x + 0x20, y + 0x20));
                auto& bit = bits.at(static_cast<int>(y >> 4), static_cast<int>(x >> 4));
                bit = passable == 0 ? 1 : 0;
                if (water == 0) {
                    if (!bit) {
                        bool hit = false;
                        const int d[8][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
                        for (const auto& e : d) {
                            const long long xx = x + e[0] * 16, yy = y + e[1] * 16;
                            if (xx >= 0 && yy >= 0 && xx < R_ - 0x21 && yy < B_ - 0x21 && class_at(xx + 0x20, yy + 0x20) == 13) {
                                hit = true;
                                break;
                            }
                        }
                        bit = hit ? 1 : 0;
                    }
                } else
                    bit = water_boundary(x, y, 13) ? 1 : 0;
            }
    }

    // 0x00545100 (cerebrivm function:terrain-check-class-boundary-at-subcell)
    bool water_boundary(long long x, long long y, int c) const {
        const long long ux = x - x % 16 + 0x20, uy = y - y % 16 + 0x20;
        const long long mx = ux % 64, my = uy % 64;
        const bool left = mx == 0, top = my == 0, right = mx == 0x30, bottom = my == 0x30;
        if (!(left || top || right || bottom)) return false;
        const long long cx = ux - mx, cy = uy - my;
        const long long icx = ux >> 6, icy = uy >> 6;
        const bool has_l = icx > 0, has_t = icy > 0, has_r = icx < (R_ >> 6), has_b = icy < (B_ >> 6);
        bool diag = true, horiz = true, vert = true;
        if (left) {
            if (top && has_l && has_t) diag = class_at(cx - 64, cy - 64) == c;
            if (bottom && has_l && has_b) diag = class_at(cx - 64, cy + 64) == c;
        }
        if (right) {
            if (top && has_r && has_t) diag = class_at(cx + 64, cy - 64) == c;
            if (bottom && has_r && has_b) diag = class_at(cx + 64, cy + 64) == c;
        }
        if (left && has_l) horiz = class_at(cx - 64, cy) == c;
        if (top && has_t) vert = class_at(cx, cy - 64) == c;
        if (right && has_r) horiz = class_at(cx + 64, cy) == c;
        if (bottom && has_b) vert = class_at(cx, cy + 64) == c;
        return !(horiz && vert && diag);
    }

    // 0x00546A80
    void footprint(const EntityFootprint& fp, long long px, long long py) {
        const long long L = 0, T = asr(s32(0 * 0xB5), 8), R = R_ - 0x20, B = asr(s32(B_ * 0xB5), 8) - 0x11F;
        const long long h0 = height(px, py);
        for (long long x = fp.minx; x <= fp.maxx; x += 16) {
            bool run = false;
            long long prev = 0;
            for (long long y = fp.miny; y <= fp.maxy; y += 16) {
                const bool on = 0 <= x && x < 2048 && 0 <= y && y < 2048 && fp.grid.at(py_index(y >> 4, fp.grid.rows), py_index(x >> 4, fp.grid.cols));
                if (!on) {
                    run = false;
                    continue;
                }
                long long sy = asr(s32((-y + 0x408) * 0xB5), 8) + (asr(s32(py * 0xB5), 8) - h0);
                long long sx = px - 0x3F8 + x;
                sx = std::min(std::max(sx, L), R);
                sy = std::min(std::max(sy, T), B);
                const auto [wx, wy] = raycast(sx, sy);
                if (0 <= wx && wx <= R_ && 0 <= wy && wy <= B_) {
                    set(wx, wy);
                    if (run)
                        for (long long yy = wy; yy <= prev; yy += 16) set(wx, yy);
                    run = true;
                    prev = wy;
                }
            }
        }
    }

    // 0x00544BD0
    void borders() {
        for (int r = 0; r < bits.rows; ++r) {
            bits.at(r, 0) = 1;
            bits.at(r, 1) = 1;
            bits.at(r, py_index((W_ - 16) >> 4, bits.cols)) = 1;
            bits.at(r, py_index((W_ - 32) >> 4, bits.cols)) = 1;
        }
        const long long top = asr(s32(0 * 0xB5), 8);
        const long long bot = asr(s32(B_ * 0xB5), 8) - 0x11F;
        for (long long x = 0; x < W_; x += 16) {
            long long y = raycast(x, top).second;
            y = y + ((-y - 16) & 16);
            long long yy = 0;
            if (y != -0x30 && y + 0x30 > -1)
                for (; yy < y + 0x30; yy += 16) bits.at(py_index(yy >> 4, bits.rows), static_cast<int>(x >> 4)) = 1;
            y = raycast(x, bot).second;
            y = y - (y & 16);
            for (yy = y - 0x20; yy < H_; yy += 16) bits.at(py_index((yy & 0xFFFFFFFFll) >> 4, bits.rows), static_cast<int>(x >> 4)) = 1;
        }
    }

    // np.packbits(bits, axis=1, bitorder="little")
    std::vector<std::uint8_t> packed() const {
        std::vector<std::uint8_t> out;
        const int bytes_per_row = (bits.cols + 7) / 8;
        out.reserve(static_cast<std::size_t>(bits.rows) * bytes_per_row);
        for (int r = 0; r < bits.rows; ++r)
            for (int b = 0; b < bytes_per_row; ++b) {
                std::uint8_t v = 0;
                for (int k = 0; k < 8 && b * 8 + k < bits.cols; ++k)
                    if (bits.at(r, b * 8 + k)) v |= static_cast<std::uint8_t>(1u << k);
                out.push_back(v);
            }
        return out;
    }

private:
    long long W_, H_, R_, B_;
    const Grid<std::uint8_t>& h_;
    const Grid<std::uint8_t>& cls_;
};

}  // namespace iberivm::rm
