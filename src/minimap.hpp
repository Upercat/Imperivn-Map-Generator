// W6 minimap: a small top-down picture of the extracted map from the game's own data.
//
// Each terrain class is the layer of the same index in data/terrains.xml, whose `minimap` attribute names a texture
// under MINIMAP/ZOOM32/TERRAIN (with %season% = Spring for a game start); the class colour is that texture's average.
// The light grid (32-unit cells, about 16 on flat ground) shades it. Markers come from the generator's object list:
// player start points (BaseTownhall members), other player settlements, outposts (forts, classes `*Outpost`) and
// the remaining neutral settlements. This is an approximation of the in-game minimap, meant to recognise a map.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "generate.hpp"
#include "pre_session.hpp"

namespace mapx {

struct Rgb {
    std::uint8_t r = 128, g = 128, b = 128;
};

// Top-down image, one 0x00RRGGBB word per pixel, rows from the top.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels;
    void set(int x, int y, Rgb c) {
        if (x >= 0 && y >= 0 && x < width && y < height)
            pixels[static_cast<std::size_t>(y) * width + x] = static_cast<std::uint32_t>(c.r) << 16 | static_cast<std::uint32_t>(c.g) << 8 | c.b;
    }
};

// What a marker shows. Neutral settlements of a generated map (classes seen in the E07 object list): library records
// owned by index 14 or 15 are independent villages; class settlements are the forts (`<race letter>Outpost`), Teuton
// tents (`TTent`, camps), teleports (`Teleport_1`, `Teleport_2`) and sites such as `Ruins1` and `Stonehenge1`.
// Forts by their class data (DATA/CLASSES): ROutpost and EOutpost "Fort" produce gold while they hold 2000;
// COutpost "Garrison" (24 macemen); IOutpost and BOutpost "Military post"; TOutpost (valkyries) and GOutpost "Trade
// post", which sell food for gold.
enum class Feature {
    PlayerStart, PlayerSettlement,
    GoldFort, CarthaginianFort, IberianFort, BritonFort, GermanFort, GallicFort, OtherFort,
    TeutonCamp, Village, Teleport, Site,
    Count
};

struct MapMarker {
    Feature feature;
    int owner = -1;
    long long x = 0, y = 0;
    std::string cls;  // class of a class settlement
};

inline Feature fort_feature(const std::string& cls) {
    if (cls == "ROutpost" || cls == "EOutpost") return Feature::GoldFort;
    if (cls == "COutpost") return Feature::CarthaginianFort;
    if (cls == "IOutpost") return Feature::IberianFort;
    if (cls == "BOutpost") return Feature::BritonFort;
    if (cls == "TOutpost") return Feature::GermanFort;
    if (cls == "GOutpost") return Feature::GallicFort;
    return Feature::OtherFort;
}

// Shape and colour of each feature, shared with the window's legend: `sides` 0 is a circle, otherwise a regular
// polygon whose first vertex lies at `rotation` degrees (0 points right, -90 up); `size` is added to the base unit.
struct FeatureStyle {
    int sides = 0;
    double rotation = 0;
    Rgb colour;
    int size = 0;
};

inline FeatureStyle feature_style(Feature f) {
    switch (f) {
    case Feature::GoldFort: return {6, 0, {245, 200, 30}, 3};
    case Feature::CarthaginianFort: return {3, -90, {60, 190, 80}, 3};
    case Feature::IberianFort: return {3, 90, {240, 120, 40}, 3};
    case Feature::BritonFort: return {5, -90, {90, 170, 235}, 3};
    case Feature::GermanFort: return {4, 45, {50, 90, 210}, 2};
    case Feature::GallicFort: return {4, 45, {20, 160, 150}, 2};
    case Feature::OtherFort: return {4, 45, {170, 170, 170}, 2};
    case Feature::TeutonCamp: return {4, 0, {150, 40, 30}, 3};
    case Feature::Village: return {0, 0, {245, 240, 225}, 0};
    case Feature::Teleport: return {0, 0, {170, 90, 230}, -1};
    case Feature::Site: return {0, 0, {150, 150, 150}, -1};
    case Feature::PlayerStart: return {0, 0, {220, 40, 40}, 4};  // drawn in the player's colour
    case Feature::PlayerSettlement:
    case Feature::Count: break;
    }
    return {0, 0, {200, 200, 200}, 0};
}

// Vertices of a feature's polygon around (cx, cy) with circumradius r.
inline std::vector<std::pair<double, double>> polygon_points(const FeatureStyle& s, double cx, double cy, double r) {
    std::vector<std::pair<double, double>> pts;
    for (int k = 0; k < s.sides; ++k) {
        const double a = (s.rotation + 360.0 * k / s.sides) * 3.14159265358979323846 / 180.0;
        pts.emplace_back(cx + r * std::cos(a), cy + r * std::sin(a));
    }
    return pts;
}

// Inside a convex polygon given in drawing order (either orientation).
inline bool inside_polygon(const std::vector<std::pair<double, double>>& p, double x, double y) {
    int sign = 0;
    for (std::size_t i = 0; i < p.size(); ++i) {
        const auto& a = p[i];
        const auto& b = p[(i + 1) % p.size()];
        const double cross = (b.first - a.first) * (y - a.second) - (b.second - a.second) * (x - a.first);
        const int s = cross > 0 ? 1 : cross < 0 ? -1 : 0;
        if (s == 0) continue;
        if (sign == 0) sign = s;
        else if (s != sign) return false;
    }
    return true;
}

// The game stores player colours as RGB555.
inline Rgb rgb555(std::uint32_t c) {
    auto expand = [](std::uint32_t v) { return static_cast<std::uint8_t>((v & 31u) * 255u / 31u); };
    return {expand(c >> 10), expand(c >> 5), expand(c)};
}

namespace detail {

// Average colour of a 24- or 32-bit uncompressed BMP; nullopt for anything else.
inline std::optional<Rgb> bmp_average(const std::vector<std::uint8_t>& b) {
    if (b.size() < 54 || b[0] != 'B' || b[1] != 'M') return std::nullopt;
    auto u32 = [&](std::size_t at) { return static_cast<std::uint32_t>(b[at] | b[at + 1] << 8 | b[at + 2] << 16 | b[at + 3] << 24); };
    const std::uint32_t offset = u32(10);
    const auto w = static_cast<std::int32_t>(u32(18)), h = static_cast<std::int32_t>(u32(22));
    const int bpp = b[28] | b[29] << 8;
    if ((bpp != 24 && bpp != 32) || u32(30) != 0 || w <= 0 || h == 0) return std::nullopt;
    const std::size_t rows = static_cast<std::size_t>(std::abs(h)), stride = (static_cast<std::size_t>(w) * (bpp / 8) + 3) & ~std::size_t{3};
    if (offset + rows * stride > b.size()) return std::nullopt;
    std::uint64_t r = 0, g = 0, bl = 0, n = 0;
    for (std::size_t y = 0; y < rows; ++y)
        for (std::size_t x = 0; x < static_cast<std::size_t>(w); ++x) {
            const std::size_t at = offset + y * stride + x * (bpp / 8);
            bl += b[at], g += b[at + 1], r += b[at + 2], ++n;
        }
    return Rgb{static_cast<std::uint8_t>(r / n), static_cast<std::uint8_t>(g / n), static_cast<std::uint8_t>(bl / n)};
}

}  // namespace detail

// Class colours (index = terrain class) from the installation's terrain list and minimap textures.
inline std::vector<Rgb> terrain_palette(const Install& install) {
    std::vector<Rgb> out;
    const auto text = install.fs().read_text("data/terrains.xml");
    static const std::regex layer(R"re(<layer\b([^>]*)>)re"), attr(R"re((\w+)\s*=\s*"([^"]*)")re");
    for (auto it = std::sregex_iterator(text.begin(), text.end(), layer); it != std::sregex_iterator(); ++it) {
        const std::string body = (*it)[1].str();
        std::map<std::string, std::string> a;
        for (auto at = std::sregex_iterator(body.begin(), body.end(), attr); at != std::sregex_iterator(); ++at) a[(*at)[1].str()] = (*at)[2].str();
        const int z = a.count("z") ? std::atoi(a["z"].c_str()) : -1;
        if (z < 0 || z > 255) continue;
        if (out.size() <= static_cast<std::size_t>(z)) out.resize(static_cast<std::size_t>(z) + 1);
        std::string tex = a["minimap"];
        if (const auto s = tex.find("%season%"); s != std::string::npos) tex.replace(s, 8, "Spring");
        const std::string path = "MINIMAP/ZOOM32/TERRAIN/" + tex;
        if (!tex.empty() && install.fs().exists(path))
            if (const auto c = detail::bmp_average(install.fs().read(path))) out[static_cast<std::size_t>(z)] = *c;
    }
    return out;
}

// Markers from the generator's object list (MapFiles::objects): settlement lines
// `settlement record <idx> <owner> <x> <y> ...` / `settlement class <name> <owner> <x> <y> ...` and member lines.
inline std::vector<MapMarker> map_markers(Install& install, const MapFiles& map) {
    std::vector<MapMarker> out;
    std::map<int, MapMarker> starts;
    std::istringstream lines(map.objects);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream f(line);
        std::string kind;
        f >> kind;
        if (kind == "settlement") {
            std::string source, id;
            int owner = -1;
            long long x = 0, y = 0;
            if (!(f >> source >> id >> owner >> x >> y)) continue;
            const bool outpost = id.size() >= 7 && id.compare(id.size() - 7, 7, "Outpost") == 0;
            Feature m = Feature::Site;
            if (owner >= 0 && owner < 8) m = Feature::PlayerSettlement;
            else if (source == "record") m = Feature::Village;
            else if (outpost) m = fort_feature(id);
            else if (id == "TTent") m = Feature::TeutonCamp;
            else if (id.rfind("Teleport", 0) == 0) m = Feature::Teleport;
            out.push_back({m, owner, x, y, source == "class" ? id : std::string()});
        } else if (kind == "member") {
            std::string cls, facing;
            long long x = 0, y = 0;
            int owner = -1;
            if (!(f >> cls >> x >> y >> facing >> owner) || owner < 0 || owner >= 8) continue;
            if (install.props().heir(cls, "BaseTownhall")) starts[owner] = {Feature::PlayerStart, owner, x, y, cls};
        }
    }
    for (auto& [owner, m] : starts) out.push_back(m);
    return out;
}

// How many markers of each feature a map has (the legend's counts).
inline std::array<int, static_cast<std::size_t>(Feature::Count)> feature_counts(const std::vector<MapMarker>& markers) {
    std::array<int, static_cast<std::size_t>(Feature::Count)> n{};
    for (const auto& m : markers) ++n[static_cast<std::size_t>(m.feature)];
    return n;
}

// Player colours of slots 0..7 from the settings (RGB555), the defaults where a slot has none.
inline std::array<Rgb, 8> player_colours(const InputFile& in) {
    static const std::array<Rgb, 8> fallback{{{220, 40, 40}, {40, 90, 220}, {40, 180, 60}, {230, 200, 40},
                                               {160, 60, 200}, {240, 130, 30}, {40, 200, 200}, {230, 230, 230}}};
    std::array<Rgb, 8> out = fallback;
    for (std::size_t k = 0; k < 8; ++k)
        if (const auto c = in.settings.players[k].color; c != 0) out[k] = rgb555(c);
    return out;
}

inline Image render_minimap(Install& install, const MapFiles& map, const InputFile& in, int px,
                            const std::vector<MapMarker>* known_markers = nullptr) {
    const int size = static_cast<int>(in.settings.random_map.map_size);
    const auto& cls = map.files.at("Terrain.terrain.grid");
    const auto& light = map.files.at("Terrain.light.grid");
    const int nc = size >> 6, nl = size >> 5;
    if (cls.size() != 20 + static_cast<std::size_t>(nc) * nc || light.size() != 20 + static_cast<std::size_t>(nl) * nl)
        throw std::runtime_error("unexpected grid sizes");
    const auto palette = terrain_palette(install);
    Image img{px, px, std::vector<std::uint32_t>(static_cast<std::size_t>(px) * px, 0)};
    for (int py = 0; py < px; ++py)
        for (int pxl = 0; pxl < px; ++pxl) {
            const long long wx = (2LL * pxl + 1) * size / (2LL * px), wy = (2LL * py + 1) * size / (2LL * px);
            const std::uint8_t c = cls[20 + static_cast<std::size_t>(wy >> 6) * nc + static_cast<std::size_t>(wx >> 6)];
            const std::uint8_t l = light[20 + static_cast<std::size_t>(wy >> 5) * nl + static_cast<std::size_t>(wx >> 5)];
            const Rgb base = c < palette.size() ? palette[c] : Rgb{};
            const double k = std::clamp(l / 16.0, 0.45, 1.35);
            auto sh = [&](std::uint8_t v) { return static_cast<std::uint8_t>(std::clamp(v * k, 0.0, 255.0)); };
            img.set(pxl, py, {sh(base.r), sh(base.g), sh(base.b)});
        }

    const auto colours = player_colours(in);
    const Rgb dark{20, 20, 20};
    const int unit = std::max(2, px / 128);
    // a filled shape with a one-pixel dark outline, centred on a world position
    auto shape = [&](const MapMarker& m, Rgb fill) {
        const FeatureStyle s = feature_style(m.feature);
        const double cx = static_cast<double>(m.x) * px / size, cy = static_cast<double>(m.y) * px / size;
        const double r = std::max(1, unit + s.size);
        const int box = static_cast<int>(r) + 3;
        const auto outer = polygon_points(s, cx, cy, r + 1.5), inner = polygon_points(s, cx, cy, r);
        for (int y = static_cast<int>(cy) - box; y <= static_cast<int>(cy) + box; ++y)
            for (int x = static_cast<int>(cx) - box; x <= static_cast<int>(cx) + box; ++x) {
                const double fx = x + 0.5, fy = y + 0.5;
                if (s.sides == 0) {
                    const double d2 = (fx - cx) * (fx - cx) + (fy - cy) * (fy - cy);
                    if (d2 <= r * r) img.set(x, y, fill);
                    else if (d2 <= (r + 1.2) * (r + 1.2)) img.set(x, y, dark);
                } else if (inside_polygon(inner, fx, fy)) {
                    img.set(x, y, fill);
                } else if (inside_polygon(outer, fx, fy)) {
                    img.set(x, y, dark);
                }
            }
    };
    const auto markers = known_markers ? *known_markers : map_markers(install, map);
    // small neutral sites first, then villages, camps and forts, players on top
    const Feature order[] = {Feature::Site, Feature::Teleport, Feature::Village, Feature::TeutonCamp, Feature::OtherFort,
                             Feature::GallicFort, Feature::GermanFort, Feature::BritonFort, Feature::IberianFort,
                             Feature::CarthaginianFort, Feature::GoldFort};
    for (const Feature f : order)
        for (const auto& m : markers)
            if (m.feature == f) shape(m, feature_style(f).colour);
    for (const auto& m : markers)
        if (m.feature == Feature::PlayerSettlement) shape(m, colours[static_cast<std::size_t>(m.owner)]);
    for (const auto& m : markers)
        if (m.feature == Feature::PlayerStart) shape(m, colours[static_cast<std::size_t>(m.owner)]);
    return img;
}

// 24-bit BMP of an image (the probe's output for review).
inline std::vector<std::uint8_t> to_bmp(const Image& img) {
    const std::size_t stride = (static_cast<std::size_t>(img.width) * 3 + 3) & ~std::size_t{3};
    const std::size_t data = stride * img.height, total = 54 + data;
    std::vector<std::uint8_t> b(total, 0);
    auto put32 = [&](std::size_t at, std::uint32_t v) { for (int k = 0; k < 4; ++k) b[at + k] = static_cast<std::uint8_t>(v >> (8 * k)); };
    b[0] = 'B', b[1] = 'M';
    put32(2, static_cast<std::uint32_t>(total));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(img.width));
    put32(22, static_cast<std::uint32_t>(-img.height));  // top-down rows
    b[26] = 1, b[28] = 24;
    put32(34, static_cast<std::uint32_t>(data));
    for (int y = 0; y < img.height; ++y)
        for (int x = 0; x < img.width; ++x) {
            const auto p = img.pixels[static_cast<std::size_t>(y) * img.width + x];
            const std::size_t at = 54 + y * stride + static_cast<std::size_t>(x) * 3;
            b[at] = static_cast<std::uint8_t>(p), b[at + 1] = static_cast<std::uint8_t>(p >> 8), b[at + 2] = static_cast<std::uint8_t>(p >> 16);
        }
    return b;
}

}  // namespace mapx
