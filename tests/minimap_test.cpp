// W6 minimap pieces without game files: RGB555 player colours and the BMP reader and writer.
#include <cstdio>
#include <string>

#include "minimap.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

}  // namespace

int main() {
    const auto red = mapx::rgb555(0x7C00), green = mapx::rgb555(0x03E0), blue = mapx::rgb555(0x001F), black = mapx::rgb555(0);
    check(red.r == 255 && red.g == 0 && red.b == 0, "RGB555 red");
    check(green.r == 0 && green.g == 255 && green.b == 0, "RGB555 green");
    check(blue.r == 0 && blue.g == 0 && blue.b == 255, "RGB555 blue");
    check(black.r == 0 && black.g == 0 && black.b == 0, "RGB555 black");

    // A 3x2 image, odd width so rows carry padding; its BMP reads back to the average colour.
    mapx::Image img{3, 2, std::vector<std::uint32_t>(6, 0)};
    img.set(0, 0, {30, 60, 90});
    img.set(1, 0, {30, 60, 90});
    img.set(2, 0, {30, 60, 90});
    img.set(0, 1, {90, 120, 150});
    img.set(1, 1, {90, 120, 150});
    img.set(2, 1, {90, 120, 150});
    img.set(5, 5, {255, 255, 255});  // outside: ignored
    const auto bmp = mapx::to_bmp(img);
    check(bmp.size() == 54 + 2 * 12, "BMP size with row padding");
    const auto avg = mapx::detail::bmp_average(bmp);
    check(avg && avg->r == 60 && avg->g == 90 && avg->b == 120, "BMP average");
    check(!mapx::detail::bmp_average({'B', 'M', 0, 0}), "a truncated BMP is refused");

    // forts by class: the two gold forts share a feature, every other race's fort has its own
    using F = mapx::Feature;
    check(mapx::fort_feature("ROutpost") == F::GoldFort && mapx::fort_feature("EOutpost") == F::GoldFort, "gold forts");
    check(mapx::fort_feature("COutpost") == F::CarthaginianFort && mapx::fort_feature("IOutpost") == F::IberianFort &&
              mapx::fort_feature("BOutpost") == F::BritonFort && mapx::fort_feature("TOutpost") == F::GermanFort &&
              mapx::fort_feature("GOutpost") == F::GallicFort && mapx::fort_feature("XOutpost") == F::OtherFort,
          "one feature per other fort");
    check(mapx::feature_style(F::GoldFort).sides == 6 && mapx::feature_style(F::TeutonCamp).sides == 4 &&
              mapx::feature_style(F::TeutonCamp).rotation == 0,
          "gold hexagon, camp diamond");

    // a hexagon of circumradius 10 around (50, 50): centre in, a far corner out, both orientations accepted
    const auto hex = mapx::polygon_points(mapx::feature_style(F::GoldFort), 50, 50, 10);
    check(hex.size() == 6 && mapx::inside_polygon(hex, 50, 50) && mapx::inside_polygon(hex, 58, 50) && !mapx::inside_polygon(hex, 59, 59),
          "hexagon inside test");
    auto reversed = hex;
    std::reverse(reversed.begin(), reversed.end());
    check(mapx::inside_polygon(reversed, 50, 50) && !mapx::inside_polygon(reversed, 40, 40), "reversed winding");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
