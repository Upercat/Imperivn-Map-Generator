// Phase handoff, isolation and failure contracts; frozen native/reference digests live in randommap_stage_test.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <tuple>
#include <vector>
#include "class_attrs_derive.hpp"
#include "rm_generate.hpp"

using namespace iberivm::rm;
using Event = std::tuple<std::string, std::string, std::vector<std::uint8_t>, std::uint32_t>;
struct Capture {
    std::vector<Event> events;
    StageSink sink() {
        return [this](const auto& name, const auto& ext, const auto& bytes, auto draws) {
            events.emplace_back(name, ext, bytes, draws);
        };
    }
};

void require(bool value, const char* why) {
    if (!value) throw std::runtime_error(why);
}

void equal_rng(const DrawLog& a, const DrawLog& b) {
    require(a.rng.state() == b.rng.state() && a.draws == b.draws && a.log.size() == b.log.size(), "RNG state differs");
    for (std::size_t i = 0; i < a.log.size(); ++i)
        require(a.log[i].lo == b.log[i].lo && a.log[i].hi == b.log[i].hi && a.log[i].value == b.log[i].value,
                "RNG draw sequence differs");
}

void check_case(const CaseSettings& cs, const iberivm::GameFS& fs, iberivm::rmdata::ClassData& classes,
                const iberivm::rmdata::SettlementLibraryFiles& files, const ClassAttrs& attrs) {
    Capture expected;
    Generator full(cs, fs, classes, files, expected.sink());
    full.set_class_attrs_table(attrs);
    full.run();

    // Different seeds interleaved phase by phase expose accidental state shared between runs.
    auto other = cs;
    other.rng_state ^= 0x9e3779b9u;
    Capture actual, other_interleaved, other_expected;
    Generator stepped(cs, fs, classes, files, actual.sink());
    Generator interleaved(other, fs, classes, files, other_interleaved.sink());
    stepped.set_class_attrs_table(attrs);
    interleaved.set_class_attrs_table(attrs);
    auto* address = &stepped.state();
    int phases = 0;
    while (stepped.advance()) {
        ++phases;
        require(&stepped.state() == address, "handoff moved the context");
        interleaved.advance();
    }
    interleaved.run();
    require(phases == (stepped.state().empty_template ? 2 : 9), "unexpected phase count");
    require(stepped.next_phase() == Generator::Phase::Complete, "pipeline did not complete");
    const auto event_count = actual.events.size();
    const auto draws = stepped.rng().draws;
    stepped.run();
    require(actual.events.size() == event_count && stepped.rng().draws == draws, "completed run repeated work");
    require(expected.events == actual.events, "stage order, bytes, type or draw checkpoints differ");
    equal_rng(full.rng(), stepped.rng());

    Generator isolated(other, fs, classes, files, other_expected.sink());
    isolated.set_class_attrs_table(attrs);
    isolated.run();
    require(other_expected.events == other_interleaved.events, "interleaved runs contaminated each other");
    equal_rng(interleaved.rng(), isolated.rng());
    // Production output filtering must not change final bytes, RNG state or cached-run results.
    GenerationResources resources(fs);
    std::vector<Event> map_events;
    for (const auto& event : expected.events) {
        const auto& name = std::get<0>(event);
        if (name.rfind("final_", 0) == 0 || name == "map_obj" || name == "objects") map_events.push_back(event);
    }
    Capture lean_capture;
    Generator lean(cs, fs, classes, files, lean_capture.sink(), {false, StageSelection::MapFiles}, &resources);
    lean.set_class_attrs_table_ref(attrs);
    int producers = 0;
    lean.run();
    require(lean_capture.events == map_events, "production output differs");
    require(lean.rng().log.empty() && lean.rng().rng.state() == full.rng().rng.state() &&
            lean.rng().draws == full.rng().draws, "disabled tracing changed RNG");
    require(lean.state().emitted_stages == map_events.size(), "diagnostic stages were still serialized");
    Capture cached_capture;
    Generator cached(cs, fs, classes, files, cached_capture.sink(), {}, &resources);
    cached.set_class_attrs_table_ref(attrs);
    cached.run();
    require(cached_capture.events == expected.events, "cached run differs");
    equal_rng(cached.rng(), full.rng());
    Generator silent(cs, fs, classes, files, [&](const auto&, const auto&, const auto&, auto) { ++producers; },
                     {false, StageSelection::None}, &resources);
    silent.set_class_attrs_table_ref(attrs);
    silent.run();
    require(producers == 0 && silent.state().emitted_bytes == 0, "silent mode emitted bytes");
    require(silent.state().h_.v == lean.state().h_.v && silent.state().cls_.v == lean.state().cls_.v &&
            silent.rng().rng.state() == lean.rng().rng.state() && silent.rng().draws == lean.rng().draws,
            "silent mode changed generation");
    require(resources.loads.decor == 1 && resources.loads.pass == 1 && resources.loads.terrain <= 1 &&
            resources.loads.trees <= 1 && resources.loads.shipyards <= 1, "installation data was reloaded");
    require(!resources.pass_workspace.v.empty(), "passability workspace was not returned");
    // Lazy producers must not be invoked at all for filtered diagnostics.
    GenerationState probe(cs, fs, classes, files, [](const auto&, const auto&, const auto&, auto) {},
                          {false, StageSelection::MapFiles}, &resources);
    probe.emit_lazy("unused_diagnostic", "u8", [&] { ++producers; return std::vector<std::uint8_t>{1}; });
    require(producers == 0, "filtered diagnostic producer still ran");
    std::cout << "PASS " << cs.template_name << " seed " << cs.rng_state << ": " << phases << " phases, "
              << actual.events.size() << " byte-identical stage outputs, " << draws << " draws\n";
}

int main(int argc, char** argv) {
    try {
        if (argc < 4) throw std::runtime_error("usage: randommap_pipeline_test <game_root> <case.json>...");
        const std::filesystem::path root(argv[1]);
        if (!std::filesystem::is_directory(root)) return 77;
        const iberivm::GameFS fs(root);
        iberivm::rmdata::ClassData classes(fs);
        const auto files = iberivm::rmdata::SettlementLibraryFiles::from_game(root);
        const auto attrs = iberivm::classattrs::attr_lists(iberivm::classattrs::derive(fs));
        CaseSettings base;
        for (int i = 2; i < argc; ++i) {
            std::ifstream input(argv[i], std::ios::binary);
            if (!input) throw std::runtime_error("case file missing");
            const std::string text((std::istreambuf_iterator<char>(input)), {});
            base = parse_case(iberivm::json::parse(text));
            check_case(base, fs, classes, files, attrs);
        }
        // The empty route bypasses template loading but still resolves random races and writes all final grids.
        base.template_path = "data/RandomMap/empty.rm.xml";
        base.template_name = "empty";
        base.size = 2048;
        base.races.assign(8, 8);
        check_case(base, fs, classes, files, attrs);

        // A phase error leaves partial data: prevent accidental continuation or repeated RNG draws.
        base.template_name = "__missing_pipeline_test_template__";
        base.template_path = "data/RandomMap/__missing_pipeline_test_template__.rm.xml";
        Generator failed(base, fs, classes, files, {});
        failed.advance();
        bool rejected = false;
        try { failed.advance(); } catch (const std::exception&) { rejected = true; }
        require(rejected && failed.next_phase() == Generator::Phase::Failed, "phase failure was not recorded");
        const auto draws = failed.rng().draws;
        rejected = false;
        try { failed.run(); } catch (const std::logic_error&) { rejected = true; }
        require(rejected && failed.rng().draws == draws, "failed run resumed on partial state");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
