// Optimized saved-game acceptance must preserve the complete mismatch fallback and candidate accounting.
#include <filesystem>
#include <iostream>
#include "extract.hpp"

void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: generation_optimization_test <game_root> <save> <mismatch_save>");
        if (!std::filesystem::is_directory(argv[1]) || !std::filesystem::exists(argv[2]) || !std::filesystem::exists(argv[3])) return 77;
        mapx::Install install{std::filesystem::path(argv[1])};
        const auto in = mapx::read_input(argv[2]);
        const auto expected = mapx::extract_map(install, in, {}, false);
        require(expected.verification == mapx::Verification::Verified, "expected a verified fixture");
        auto saved = mapx::saved_grids(in);
        bool called = false;
        const auto accepted = mapx::generate_map_candidate(install, in, expected.pre, [&](const auto& state) {
            called = true;
            return mapx::required_terrain_equal(state, saved);
        });
        require(called && accepted && accepted->files == expected.map.files && accepted->objects == expected.map.objects &&
                accepted->draws == expected.map.draws, "accepted map differs from complete generation");
        saved.at("Terrain.height.grid").at(20) ^= 1;
        called = false;
        const auto rejected = mapx::generate_map_candidate(install, in, expected.pre, [&](const auto& state) {
            called = true;
            return mapx::required_terrain_equal(state, saved);
        });
        require(called && !rejected, "wrong terrain did not reject before output");
        // Header validation matters as well as payload equality.
        saved = mapx::saved_grids(in);
        saved.at("Terrain.terrain.grid").at(8) ^= 1;
        require(!mapx::generate_map_candidate(install, in, expected.pre, [&](const auto& state) {
            return mapx::required_terrain_equal(state, saved);
        }), "wrong grid header accepted");

        const auto mismatch = mapx::read_input(argv[3]);
        const auto complete = mapx::extract_map(install, mismatch, {}, false);
        const auto early = mapx::extract_map(install, mismatch);
        require(complete.verification == mapx::Verification::Mismatch && early.verification == complete.verification,
                "mismatch verdict changed");
        require(complete.map.files == early.map.files && complete.map.objects == early.map.objects &&
                complete.map.draws == early.map.draws && complete.pre.session_state == early.pre.session_state &&
                complete.tried == early.tried && complete.candidates == early.candidates, "mismatch fallback changed");
        require(complete.tried > 1 && early.early_rejected == early.tried - 1, "later candidates were not rejected early");
        const auto& loads = install.generation_resources().loads;
        require(loads.decor == 1 && loads.terrain == 1 && loads.pass == 1 && loads.trees <= 1 && loads.shipyards <= 1, "installation cache reloaded data");
        std::cout << "PASS accepted/rejected terrain, headers, cache reuse and " << early.early_rejected
                  << " early rejections with an identical complete fallback\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
