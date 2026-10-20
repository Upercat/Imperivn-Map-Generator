#include <filesystem>
#include <iostream>
#include <fstream>
#include <set>
#include "gallery.hpp"
#include "extract.hpp"

void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
int main(int argc, char** argv) {
    try {
        require(mapx::gallery_seed("FFFFFFFF") == 0xffffffffu && mapx::gallery_seed("0x12a") == 298u &&
                mapx::gallery_seed(" 00000000 ") == 0u, "valid seed rejected");
        require(!mapx::gallery_seed("") && !mapx::gallery_seed("100000000") && !mapx::gallery_seed("-1") &&
                !mapx::gallery_seed("xyz"), "invalid seed accepted");
        std::set<std::uint32_t> seeds;
        for (std::size_t i = 0; i < 9; ++i) seeds.insert(mapx::gallery_card_seed(0xfffffffcu, i));
        require(seeds.size() == 9 && seeds.contains(0), "batch seed wrap repeats a seed");
        if ((argc != 2 && argc != 6) || !std::filesystem::is_directory(argv[1])) return 77;
        mapx::Install install{std::filesystem::path(argv[1])};
        const auto templates = mapx::gallery_templates(install.fs());
        const auto it = std::find_if(templates.begin(), templates.end(), [](const auto& t) {
            return mapx::template_name(t.path) == "CONTINENT";
        });
        require(it != templates.end() && it->max_players[0] == 2 && it->max_players[1] == 8, "catalogue limits lost");
        require(std::none_of(templates.begin(), templates.end(), [](const auto& item) {
            return mapx::template_name(item.path) == "TOURNAMENT";
        }), "unsupported tournament bitmap was offered in the catalogue");
        mapx::GalleryRequest request{*it, 0, 2, 8, 0x12345678u};
        if (argc == 6 && std::string(argv[2]) == "--export") {
            const auto seed = mapx::gallery_seed(argv[3]);
            require(seed.has_value(), "invalid test seed");
            request.base_seed = *seed;
            const auto generated = mapx::generate_gallery_map(install, request, 8, argv[5], 144);
            require(generated.has_value(), "test export cancelled");
            std::ofstream output(argv[4], std::ios::binary);
            output.write(reinterpret_cast<const char*>(generated->volume.data()), static_cast<std::streamsize>(generated->volume.size()));
            return output ? 0 : 1;
        }
        const auto settings = mapx::gallery_input(request, request.base_seed);
        require(mapx::active_slots(settings.settings.random_map).size() == 2 && settings.settings.sync_seed == request.base_seed,
                "request settings changed");
        auto invalid = request;
        invalid.players = 3;
        bool refused = false;
        try { mapx::gallery_input(invalid, 0); } catch (const std::invalid_argument&) { refused = true; }
        require(refused, "template player limit ignored");
        int calls = 0;
        require(!mapx::generate_gallery_map(install, request, 0, "english", 96, [&] { ++calls; return true; }) && calls > 0,
                "cancellation did not stop the batch");
        const auto a = mapx::generate_gallery_map(install, request, 0, "english", 96);
        const auto b = mapx::generate_gallery_map(install, request, 1, "english", 96);
        const auto again = mapx::generate_gallery_map(install, request, 0, "english", 96);
        require(a && b && again && !a->volume.empty() && a->minimap.pixels.size() == 96 * 96, "gallery result incomplete");
        require(a->volume == again->volume && a->minimap.pixels == again->minimap.pixels, "same settings/seed changed the map");
        require(a->volume != b->volume, "different seeds produced an identical fixture map");
        require(mapx::hpfs::validate(a->volume).empty(), "exported BFHP invalid");
        const auto recovered = mapx::extract_map(install, a->input);
        require(mapx::pack_map(install, mapx::assemble_map(install, recovered.map, a->input, recovered.pre, "english")) == a->volume,
                "gallery map differs from recovery path for the same settings/seed");
        std::cout << "PASS catalogue, seed wrap, cancellation, reproducible thumbnails/exports and recovery equivalence\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
