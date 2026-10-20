// W3 generation inside the tool: the user's game installation, the case built from a replay or saved game, and one
// run of Iberivm's random-map generator that keeps only the map files.
#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "class_attrs_derive.hpp"
#include "game_fs.hpp"
#include "input_reader.hpp"
#include "pre_session.hpp"
#include "randommap_data.hpp"
#include "rm_generate.hpp"

namespace mapx {

// Refusals found while preparing a generation; the window maps each to a translated message.
enum class InstallProblem {
    None,
    NotAGameFolder,     // no DATA/CONST.INI, or the settlement pack cannot be read
    ModdedConstants,    // CONST.INI values the generator relies on differ from the shipped game
    ModdedData,         // shipped data the generator checks (settlement library) differs
    TemplateMissing,    // the input's template is not in this installation (a mod's own template)
    GenerationFailed,   // the generator stopped: a template format or a rare path it does not model
};

class InstallError : public std::runtime_error {
public:
    InstallError(InstallProblem p, const std::string& what) : std::runtime_error(what), problem(p) {}
    InstallProblem problem;
};

// CONST.INI values compiled into the generator (shipped game; see the CONST.INI comments in Iberivm's
// src/sim/randommap). A mod that changes them would generate another map, so the tool refuses instead.
struct ExpectedConstant {
    const char* section;
    const char* key;
    const char* value;
};
inline constexpr ExpectedConstant kGeneratorConstants[] = {
    {"RandomMap", "BuildingInfSeed", "46"},   {"RandomMap", "BuildingInfFade", "3"},
    {"RandomMap", "WaterInfSeed", "30"},      {"RandomMap", "WaterInfFade", "2"},
    {"RandomMap", "BorderInfSeed", "30"},     {"RandomMap", "BorderInfFade", "3"},
    {"RandomMap", "MountainInfSeed", "40"},   {"RandomMap", "MountainInfFade", "3"},
    {"RandomMap", "pxBaseVillageDist", "2000"}, {"RandomMap", "pxVillageDistAdd", "50"},
    {"RandomMap", "SettlementsNeighborhood", "100"}, {"RandomMap", "SYMinDistSameBound", "48"},
    {"RandomMap", "SYMinDistDiffBound", "48"}, {"RandomMap", "MinLakeIslandArea", "1024"},
    {"RandomMap", "TeleportMinDist", "33"},   {"RandomMap", "TemplatesFolder", "data/RandomMap"},
    {"GamePlay", "IndependentVillagePop", "50"},
};

namespace detail {

inline std::string lower_ascii(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// First value of [section] key, without its `;` comment; keys and sections without case.
inline std::optional<std::string> ini_value(const std::string& text, const std::string& section, const std::string& key) {
    const std::string want_section = lower_ascii(section), want_key = lower_ascii(key);
    std::string current;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';') continue;
        if (line.front() == '[') {
            current = lower_ascii(trim(line.substr(1, line.find(']') - 1)));
            continue;
        }
        if (current != want_section) continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos || lower_ascii(trim(line.substr(0, eq))) != want_key) continue;
        std::string value = line.substr(eq + 1);
        if (const auto semi = value.find(';'); semi != std::string::npos) value.resize(semi);
        return trim(value);
    }
    return std::nullopt;
}

}  // namespace detail

inline std::string constants_problem(const std::string& const_ini) {
    for (const auto& c : kGeneratorConstants) {
        const auto v = detail::ini_value(const_ini, c.section, c.key);
        if (!v) return std::string("CONST.INI [") + c.section + "] " + c.key + " is missing";
        if (detail::lower_ascii(*v) != detail::lower_ascii(c.value))
            return std::string("CONST.INI [") + c.section + "] " + c.key + " = " + *v + " (shipped " + c.value + ")";
    }
    return {};
}

// The installation, read once and reused for every generation.
class Install {
public:
    explicit Install(const std::filesystem::path& root) : root_(root), fs_(make_fs(root)), classes_(*fs_), resources_(*fs_) {
        if (!fs_->exists("DATA/CONST.INI")) throw InstallError(InstallProblem::NotAGameFolder, "no DATA/CONST.INI");
        const auto const_ini = fs_->read_text("DATA/CONST.INI");
        if (auto why = constants_problem(const_ini); !why.empty()) throw InstallError(InstallProblem::ModdedConstants, why);
        names_ = read_name_tables(const_ini);
        try {
            library_ = std::make_unique<iberivm::rmdata::SettlementLibraryFiles>(iberivm::rmdata::SettlementLibraryFiles::from_game(root_));
            // the library reader checks the files the generator was verified with
            (void)library_->read("map.obj.xml");
        } catch (const std::exception& e) {
            throw InstallError(InstallProblem::ModdedData, std::string("settlement library: ") + e.what());
        }
        attrs_ = iberivm::classattrs::attr_lists(iberivm::classattrs::derive(*fs_));
    }
    Install(const Install&) = delete;
    Install& operator=(const Install&) = delete;

    const std::filesystem::path& root() const { return root_; }
    const iberivm::GameFS& fs() const { return *fs_; }
    iberivm::rmdata::ClassData& classes() { return classes_; }
    iberivm::rmdata::ClassProps& props() { return resources_.props; }
    iberivm::rm::GenerationResources& generation_resources() { return resources_; }
    const iberivm::rmdata::SettlementLibraryFiles& library() const { return *library_; }
    const iberivm::rm::ClassAttrs& class_attrs() const { return attrs_; }
    const NameTables& names() const { return names_; }

private:
    static std::unique_ptr<iberivm::GameFS> make_fs(const std::filesystem::path& root) {
        try {
            return std::make_unique<iberivm::GameFS>(root);
        } catch (const std::exception& e) {
            throw InstallError(InstallProblem::NotAGameFolder, e.what());
        }
    }
    std::filesystem::path root_;
    std::unique_ptr<iberivm::GameFS> fs_;
    iberivm::rmdata::ClassData classes_;
    iberivm::rm::GenerationResources resources_;
    std::unique_ptr<iberivm::rmdata::SettlementLibraryFiles> library_;
    iberivm::rm::ClassAttrs attrs_;
    NameTables names_;
};

// The generator's case from a random-map input and its session-begin state. A game start (replay or skirmish save)
// runs the generation as a game start (respawning item holders draw) and the lobby forces spring.
inline iberivm::rm::CaseSettings make_case(const InputFile& in, const PreSession& pre) {
    const auto& rm = in.settings.random_map;
    iberivm::rm::CaseSettings c;
    c.template_path = rm.template_path;
    // tools/randommap: Path(template).stem.split(".")[0]
    std::string stem = rm.template_path.substr(rm.template_path.find_last_of("/\\") + 1);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot != 0) stem.resize(dot);
    c.template_name = stem.substr(0, stem.find('.'));
    c.size = static_cast<int>(rm.map_size);
    for (const auto t : rm.teams) c.teams.push_back(t == kNoTeam ? -1 : t);
    for (const auto r : pre.races) c.races.push_back(r);
    for (std::size_t i = 0; i < rm.flags.size(); ++i)
        if (rm.flags[i] != 0) c.flags[static_cast<int>(0x38 + 4 * i)] = static_cast<int>(rm.flags[i]);
    c.rng_state = pre.session_state;
    c.game_start = true;
    c.season = "spring";
    return c;
}

// The map as the editor saves it under Maps/1: Terrain.<grid>.grid and map.obj.xml.
struct MapFiles {
    std::map<std::string, std::vector<std::uint8_t>> files;
    std::string objects;  // the generator's object list after PlaceObjects (stage `objects`), one line per object
    std::uint32_t draws = 0;
    double seconds = 0;
};

using TerrainAcceptance = std::function<bool(const iberivm::rm::GenerationState&)>;

// Called only at the final terrain boundary, after shipyards and before passability/serialization.
inline std::optional<MapFiles> generate_map_candidate(Install& install, const InputFile& in, const PreSession& pre,
                                                     const TerrainAcceptance& accept = {}, const std::function<bool()>& cancelled = {}) {
    if (!install.fs().exists(in.settings.random_map.template_path))
        throw InstallError(InstallProblem::TemplateMissing, "template " + in.settings.random_map.template_path);
    const auto t0 = std::chrono::steady_clock::now();
    MapFiles out;
    auto sink = [&](const std::string& name, const std::string&, const std::vector<std::uint8_t>& bytes, std::uint32_t) {
        if (name.rfind("final_", 0) == 0) out.files["Terrain." + name.substr(6) + ".grid"] = bytes;
        else if (name == "map_obj") out.files["map.obj.xml"] = bytes;
        else if (name == "objects") out.objects.assign(bytes.begin(), bytes.end());
    };
    // The generator keeps references to its inputs: the case must outlive it.
    const auto cs = make_case(in, pre);
    iberivm::rm::Generator gen(cs, install.fs(), install.classes(), install.library(), sink,
        {false, iberivm::rm::StageSelection::MapFiles}, &install.generation_resources());
    gen.set_class_attrs_table_ref(install.class_attrs());
    try {
        while (gen.next_phase() != iberivm::rm::Generator::Phase::Complete) {
            if (cancelled && cancelled()) return std::nullopt;
            if (gen.next_phase() == iberivm::rm::Generator::Phase::Output && accept && !accept(gen.state())) return std::nullopt;
            gen.advance();
        }
    } catch (const std::exception& e) {
        throw InstallError(InstallProblem::GenerationFailed, e.what());
    }
    out.draws = gen.rng().draws;
    out.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return out;
}

inline MapFiles generate_map(Install& install, const InputFile& in, const PreSession& pre) {
    return std::move(*generate_map_candidate(install, in, pre));
}

}  // namespace mapx
