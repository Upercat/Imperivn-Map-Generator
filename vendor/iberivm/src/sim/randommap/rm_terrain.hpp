// Heights, light, mountains and terrain classes (reference: heights.py, mountains.py,
// terrain_classes.py and the post_objects / field_b_and_terrain steps of pipeline.py).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include "rm_common.hpp"

namespace iberivm::rm {

// 0x005824A7..0x005825EB: heights = clamp(trunc(A*255), 0, min(up + 21, 255)) (P16).
inline Grid<std::uint8_t> materialize_heights(const Grid<float>& a) {
    const int n = a.rows;
    Grid<std::uint8_t> out(n, n);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            const auto v = static_cast<long long>(std::trunc(static_cast<double>(a.at(r, c)) * 255.0));
            const long long cap = r == 0 ? 255 : std::min<long long>(out.at(r - 1, c) + 21, 255);
            out.at(r, c) = static_cast<std::uint8_t>(v < 0 ? 0 : std::min(v, cap));
        }
    return out;
}

// 0x00544E10 light: h - ((h below + h right) >> 1) + 16, clipped to [0, 21]; last row and column 16.
inline Grid<std::uint8_t> light_native(const Grid<std::uint8_t>& h) {
    const int n = h.rows;
    Grid<std::uint8_t> out(n, n, 16);
    for (int r = 0; r + 1 < n; ++r)
        for (int c = 0; c + 1 < n; ++c) {
            const int v = h.at(r, c) - ((h.at(r + 1, c) + h.at(r, c + 1)) >> 1) + 16;
            out.at(r, c) = static_cast<std::uint8_t>(std::clamp(v, 0, 21));
        }
    return out;
}

// 0x0054F860(pt, h, NULL): max-stamp a 20-per-32px Manhattan diamond into height bytes.
inline void mountain_stamp(Grid<std::uint8_t>& h, const long long rect[4], long long px, long long py, long long val_h) {
    const long long x0 = std::max(rect[0], px - 0x180), x1 = std::min(rect[2], px + 0x180);
    const long long y0 = std::max(rect[1], py - 0x180), y1 = std::min(rect[3], py + 0x180);
    for (long long y = y0; y < y1; y += 32)
        for (long long x = x0; x < x1; x += 32) {
            const long long dist = (std::llabs(py - y) + std::llabs(px - x)) / 32;  // CDQ/AND 0x1F/SAR 5 on a non-negative sum
            const long long val = val_h - dist * 20;
            auto& cur = h.at(static_cast<int>(y >> 5), static_cast<int>(x >> 5));
            if (cur < val) {  // CMP EBX,EAX ; JGE skip (signed)
                if (val > 255) throw std::runtime_error("mountain stamp above 255");
                cur = static_cast<std::uint8_t>(val);
            }
        }
}

// 0x0054FB60(&p0, h0, &p1, h1, map)
inline void mountain_segment(Grid<std::uint8_t>& h, const long long rect[4], std::pair<long long, long long> p0, long long h0,
                             std::pair<long long, long long> p1, long long h1, DrawLog& rng) {
    const long long dx = p1.first - p0.first, dy = p1.second - p0.second;
    const long long ln = isqrt(dx * dx + dy * dy);  // 0x0067C3D0
    const long long stepx = tdiv(dx * 150, ln), stepy = tdiv(dy * 150, ln);
    const long long steph = tdiv((h1 - h0) * 150, ln);
    const long long n = tdiv(ln, 150);
    long long offx = 0, offy = 0, hh = h0;
    for (long long i = 0; i < n; ++i) {
        const long long cx = p0.first + offx, cy = p0.second + offy;
        const long long jy = rng(-80, 80);  // 0x0054FC23 -> y
        const long long jx = rng(-80, 80);  // 0x0054FC40 -> x
        mountain_stamp(h, rect, cx + jx, cy + jy, hh);
        for (int k = 0; k < 8; ++k) {
            const long long a = rng(-350, 350);   // 0x0054FC8C -> y
            const long long bb = rng(-350, 350);  // 0x0054FCAF -> x
            const long long d = isqrt(a * a + bb * bb);
            const long long hs = hh - static_cast<long long>((static_cast<std::uint64_t>(d * hh) & 0xFFFFFFFFull) / 700);  // MUL magic, SHR 8
            mountain_stamp(h, rect, cx + bb, cy + a, hs);
        }
        offx += stepx, offy += stepy, hh += steph;
    }
}

// --- terrain classes (terrain_classes.py) ----------------------------------------------------------

struct MixTable {
    std::vector<int> ids;
    std::vector<long long> cum;
};

// 0x00581470 + 0x005812A0: per race group ids/weights -> cum[0]=0, cum[i+1]=cum[i]+trunc(w*1000/sum), last=1001.
inline std::vector<MixTable> native_tables(const std::string& text_in) {
    static const char* const races[8] = {"Gaul", "RepublicanRome", "Carthage", "Iberia", "ImperialRome", "Britain", "Egypt", "Germany"};
    std::string text;
    for (std::size_t p = 0;;) {  // re.sub(r"<!--.*?-->", "", text, flags=re.S)
        const auto b = text_in.find("<!--", p);
        if (b == std::string::npos) {
            text += text_in.substr(p);
            break;
        }
        const auto e = text_in.find("-->", b + 4);
        if (e == std::string::npos) {
            text += text_in.substr(p);
            break;
        }
        text += text_in.substr(p, b - p);
        p = e + 3;
    }
    std::vector<std::vector<int>> ids(8), ws(8);
    const std::string open = "<terrain-mix race=\"";
    for (std::size_t p = 0;;) {
        const auto b = text.find(open, p);
        if (b == std::string::npos) break;
        const auto qe = text.find('"', b + open.size());
        if (qe == std::string::npos || qe == b + open.size() || text.compare(qe, 2, "\">") != 0) {
            p = b + 1;
            continue;
        }
        const auto race = text.substr(b + open.size(), qe - b - open.size());
        const auto end = text.find("</terrain-mix>", qe + 2);
        if (end == std::string::npos) break;
        const auto body = text.substr(qe + 2, end - qe - 2);
        int g = -1;
        for (int i = 0; i < 8; ++i)
            if (race == races[i]) g = i;
        if (g < 0) throw std::runtime_error("TerrainMix.xml: unknown race " + race);
        // re.findall(r'<terrain id="(\d+)"[^>]*height="(\d+)"', body)
        for (std::size_t q = 0;;) {
            const std::string t_open = "<terrain id=\"";
            const auto tb = body.find(t_open, q);
            if (tb == std::string::npos) break;
            q = tb + 1;
            auto d = tb + t_open.size();
            while (d < body.size() && body[d] >= '0' && body[d] <= '9') ++d;
            if (d == tb + t_open.size() || d >= body.size() || body[d] != '"') continue;
            const auto tid = std::stoi(body.substr(tb + t_open.size(), d - tb - t_open.size()));
            auto gt = body.find('>', d + 1);
            if (gt == std::string::npos) gt = body.size();
            // greedy [^>]*: the last height="digits" before the tag end
            long long w = -1;
            std::size_t found_end = 0;
            for (std::size_t s = d + 1; s < gt;) {
                const auto hpos = body.find("height=\"", s);
                if (hpos == std::string::npos || hpos >= gt) break;
                auto e = hpos + 8;
                while (e < body.size() && body[e] >= '0' && body[e] <= '9') ++e;
                if (e > hpos + 8 && e < body.size() && body[e] == '"') w = std::stoll(body.substr(hpos + 8, e - hpos - 8)), found_end = e + 1;
                s = hpos + 1;
            }
            if (w < 0) continue;
            ids[static_cast<std::size_t>(g)].push_back(tid);
            ws[static_cast<std::size_t>(g)].push_back(static_cast<int>(w));
            q = found_end;
        }
        p = end + 14;
    }
    std::vector<MixTable> out(8);
    for (int g = 0; g < 8; ++g) {
        long long total = 0;
        for (int w : ws[static_cast<std::size_t>(g)]) total += w;
        auto& t = out[static_cast<std::size_t>(g)];
        t.ids = ids[static_cast<std::size_t>(g)];
        t.cum = {0};
        for (int w : ws[static_cast<std::size_t>(g)])
            t.cum.push_back(static_cast<long long>(static_cast<double>(w * 1000) / static_cast<double>(total)) + t.cum.back());
        t.cum.back() = 1001;
    }
    return out;
}

// 0x0057DEC0: v=max(v,0), min(v,998); first i with cum[i]<=v<cum[i+1]; fallback ids[0].
inline int mix_select(const MixTable& t, long long v) {
    v = std::max(v, 0ll);
    v = std::min(v, 998ll);
    for (std::size_t i = 0; i + 1 < t.cum.size(); ++i)
        if (t.cum[i] <= v && v < t.cum[i + 1]) return t.ids[i];
    return t.ids.at(0);
}

// 0x0057CF70(dst, src, r): mask from the pre-pass grid; window clipped to the grid; any != src -> dst.
inline Grid<std::uint8_t> erode_native(const Grid<std::uint8_t>& g, int dst, int src, int r) {
    const int n = g.rows;
    auto out = g;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            if (g.at(y, x) != src) continue;
            const int y0 = std::max(y - r, 0), y1 = std::min(y + r, n - 1), x0 = std::max(x - r, 0), x1 = std::min(x + r, n - 1);
            bool any = false;
            for (int yy = y0; yy <= y1 && !any; ++yy)
                for (int xx = x0; xx <= x1; ++xx)
                    if (g.at(yy, xx) != src) {
                        any = true;
                        break;
                    }
            if (any) out.at(y, x) = static_cast<std::uint8_t>(dst);
        }
    return out;
}

// 0x0057D240(a1, a2, t3, t4, t5): in place, rows 1..n-2 outer, cols 1..n-2 inner, 3x3 incl. centre.
inline void transition_native(Grid<std::uint8_t>& g, int a1, int a2, const int t[3]) {
    const int n = g.rows;
    for (int y = 1; y < n - 1; ++y)
        for (int x = 1; x < n - 1; ++x) {
            if (g.at(y, x) != a1) continue;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int v = g.at(y + dy, x + dx);
                    if (v == t[0] || v == t[1] || v == t[2]) g.at(y, x) = static_cast<std::uint8_t>(a2);
                }
        }
}

}  // namespace iberivm::rm
