// Internal benchmark: phase timing excludes input decoding, installation setup, packing and final hashing.
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include "class_attrs_derive.hpp"
#include "rm_generate.hpp"
#include "sha256.hpp"

int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::runtime_error("usage: map-generation-benchmark <game_root> <case.json> <runs>");
        const iberivm::GameFS fs{std::filesystem::path(argv[1])};
        iberivm::rmdata::ClassData classes(fs);
        const auto files = iberivm::rmdata::SettlementLibraryFiles::from_game(argv[1]);
        const auto attrs = iberivm::classattrs::attr_lists(iberivm::classattrs::derive(fs));
        std::ifstream input(argv[2]);
        const std::string text((std::istreambuf_iterator<char>(input)), {});
        const auto cs = iberivm::rm::parse_case(iberivm::json::parse(text));
#ifdef OPTIMIZED_GENERATION
        iberivm::rm::GenerationResources resources(fs);
#endif
        for (int run = 0; run < std::stoi(argv[3]); ++run) {
            std::map<std::string, std::vector<std::uint8_t>> outputs;
            std::size_t bytes_emitted = 0;
            auto sink = [&](const auto& name, const auto&, const auto& bytes, auto) {
                bytes_emitted += bytes.size();
                if (name.rfind("final_", 0) == 0 || name == "map_obj" || name == "objects") outputs[name] = bytes;
            };
            const auto t0 = std::chrono::steady_clock::now();
            iberivm::rm::Generator gen(cs, fs, classes, files, sink
#ifdef OPTIMIZED_GENERATION
                , {false, iberivm::rm::StageSelection::MapFiles}, &resources
#endif
            );
            gen.set_class_attrs_table(attrs);
            std::array<double, 9> phases{};
            while (gen.next_phase() != iberivm::rm::Generator::Phase::Complete) {
                const auto phase = static_cast<std::size_t>(gen.next_phase());
                const auto start = std::chrono::steady_clock::now();
                gen.advance();
                phases.at(phase) += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            }
            const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::string digests;
            for (const auto& [name, bytes] : outputs) digests += name + ":" + iberivm::sha256_hex(bytes) + "\n";
            std::cout << std::setprecision(9) << "{\"run\":" << run << ",\"seconds\":" << elapsed
                      << ",\"emitted_bytes\":" << bytes_emitted << ",\"rng_draws\":" << gen.rng().draws
                      << ",\"rng_log_entries\":" << gen.rng().log.size() << ",\"output_digest\":\""
                      << iberivm::sha256_hex(std::vector<std::uint8_t>(digests.begin(), digests.end())) << "\",\"phase_seconds\":[";
            for (std::size_t i = 0; i < phases.size(); ++i) std::cout << (i ? "," : "") << phases[i];
            std::cout << "]}" << std::endl;
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
