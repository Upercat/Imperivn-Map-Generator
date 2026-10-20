// Exercise every offered template and size at its maximum supported player count.
#include <chrono>
#include <filesystem>
#include <iostream>
#include "gallery.hpp"
int main(int argc, char** argv) {
    if (argc != 2 || !std::filesystem::is_directory(argv[1])) return 77;
    try {
        mapx::Install install(argv[1]);
        const auto templates = mapx::gallery_templates(install.fs());
        if (templates.empty()) throw std::runtime_error("empty catalogue");
        int tested = 0, failed = 0;
        for (const auto& item : templates) for (int size = 0; size < 3; ++size) {
            if (item.max_players[size] < 2) continue;
            const auto start = std::chrono::steady_clock::now();
            try {
                mapx::GalleryRequest request{item, size, item.max_players[size], tested % 2 ? 8 : tested % 8, 0x12345678};
                const auto first = mapx::generate_gallery_map(install, request, 0, "english", 96);
                const auto again = mapx::generate_gallery_map(install, request, 0, "english", 96);
                if (!first || !again || first->volume.empty() || first->volume != again->volume ||
                    first->minimap.pixels != again->minimap.pixels || !mapx::hpfs::validate(first->volume).empty())
                    throw std::runtime_error("non-reproducible or invalid result");
                const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::cout << "PASS " << item.path << " size=" << size << " players=" << request.players <<
                    " race=" << request.race << " bytes=" << first->volume.size() << " seconds=" << seconds << std::endl;
            } catch (const std::exception& e) { ++failed; std::cerr << "FAIL " << item.path << " size=" << size << ": " << e.what() << std::endl; }
            ++tested;
        }
        std::cout << "Matrix: " << tested << " variants, " << failed << " failures" << std::endl;
        return failed ? 1 : 0;
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
