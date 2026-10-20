// Random-map generation session 0x00587E60, stage by stage (reference and verifier:
// tools/randommap/pipeline.py and build_map.py). Every stage reports its output through the
// StageSink in the reference's order; the primary-RNG draw log is kept in DrawLog.
#pragma once
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "json_lite.hpp"
#include "randommap_data.hpp"
#include "randommap_props.hpp"
#include "randommap_rules.hpp"
#include "rm_common.hpp"
#include "rm_fields.hpp"
#include "rm_library.hpp"
#include "rm_mapobj.hpp"
#include "rm_noise.hpp"
#include "rm_objects.hpp"
#include "rm_passability.hpp"
#include "rm_shipyards.hpp"
#include "rm_template.hpp"
#include "rm_terrain.hpp"
#include "rm_trees.hpp"
#include "rm_generation_resources.hpp"

namespace iberivm::rm {

struct CaseSettings {
    std::string template_name;  // e.g. "Continent"
    std::string template_path;
    int size = 0;
    std::vector<int> teams;  // per slot, -1 = unused
    std::vector<int> races;  // per slot (8 = random, 255 = unused)
    std::map<int, int> flags;
    std::uint32_t rng_state = 0;
    bool game_start = false;
    std::string season = "spring";
};

inline CaseSettings parse_case(const json::Value& cap) {
    CaseSettings c;
    const auto& rms = cap.at("rms");
    c.template_path = rms.at("template").as_string();
    // Path(template).stem.split(".")[0]
    auto stem = c.template_path.substr(c.template_path.find_last_of("/\\") == std::string::npos ? 0 : c.template_path.find_last_of("/\\") + 1);
    if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot != 0) stem = stem.substr(0, dot);
    c.template_name = stem.substr(0, stem.find('.'));
    c.size = static_cast<int>(rms.at("map_size").as_int());
    for (const auto& b : rms.at("teams_by_slot_0x28").items) c.teams.push_back(b.as_int() != 255 ? static_cast<int>(b.as_int()) : -1);
    for (const auto& r : rms.at("races_by_slot_0x30").items) c.races.push_back(static_cast<int>(r.as_int()));
    if (const auto* f = rms.find("flags_0x38_0x64"))
        for (const auto& [k, v] : f->members) c.flags[static_cast<int>(std::stol(k, nullptr, 16))] = static_cast<int>(v.as_int());
    c.rng_state = static_cast<std::uint32_t>(std::stoul(cap.at("rng_primary_session_begin").as_string(), nullptr, 16));
    if (const auto* g = cap.find("game_start")) c.game_start = g->truthy();
    if (const auto* se = rms.find("season")) c.season = se->as_string();
    return c;
}

// Capture association file of ../iberivm_oracle/randommap (schema imperivm.randommap.association/v1): the
// effective random-map settings block (+0x28 team bytes, +0x30 race bytes, +0x38..+0x64 no_* dwords, null =
// unused slot 255), the primary state at session begin, and the template's bitmap folder, which names the
// template: the data/RandomMap/*.rm.xml whose <bitmap file> is that folder (one template per folder).
inline CaseSettings parse_association(const json::Value& asc, const GameFS& fs) {
    CaseSettings c;
    const auto& cfg = asc.at("effective_configuration");
    c.size = static_cast<int>(std::stol(cfg.at("map_size_raw").as_string(), nullptr, 16));
    for (const auto& b : cfg.at("players_by_slot").items) c.teams.push_back(b.is_null() || b.as_int() == 255 ? -1 : static_cast<int>(b.as_int()));
    for (const auto& r : cfg.at("races_by_slot").items) c.races.push_back(r.is_null() ? 255 : static_cast<int>(r.as_int()));
    if (const auto* f = cfg.find("flags_dword_offsets"))
        for (const auto& [k, v] : f->members) c.flags[static_cast<int>(std::stol(k, nullptr, 16))] = static_cast<int>(v.as_int());
    c.rng_state = static_cast<std::uint32_t>(std::stoul(asc.at("rng").at("primary").at("state_at_session_begin").as_string(), nullptr, 16));
    const auto want = GameFS::key(asc.at("template").at("configured_path").as_string());
    for (const auto& f : fs.list_files("data/RandomMap")) {
        if (f.size() < 7 || f.compare(f.size() - 7, 7, ".RM.XML") != 0) continue;
        const auto t = rmdata::load_template(fs, f.substr(f.rfind('/') + 1, f.size() - f.rfind('/') - 1 - 7));
        if (GameFS::key(t.bitmap) != want) continue;
        if (!c.template_path.empty()) throw std::runtime_error("two templates use the bitmap folder " + want);
        c.template_path = f;
        c.template_name = f.substr(f.rfind('/') + 1, f.size() - f.rfind('/') - 1 - 7);
    }
    if (c.template_path.empty()) throw std::runtime_error("no template uses the bitmap folder " + want);
    return c;
}

inline CaseSettings parse_case_or_association(const json::Value& doc, const GameFS& fs) {
    const auto* schema = doc.find("schema");
    if (schema && schema->kind == json::Value::Kind::string && schema->as_string() == "imperivm.randommap.association/v1")
        return parse_association(doc, fs);
    return parse_case(doc);
}

// One run owns this context. Each phase receives it explicitly and leaves its outputs here.
// Library object attributes point into this context: its address must remain stable between phases.
using ObjectObserver = std::function<void(bool, const std::vector<MapObject>&, const std::vector<Settlement>&)>;

enum class StageSelection { All, MapFiles, None };
struct GenerationOptions {
    bool record_rng = true;
    StageSelection stages = StageSelection::All;
};

struct GenerationState {
    GenerationState(const CaseSettings& cs, const GameFS& fs, rmdata::ClassData& classes,
                    const rmdata::SettlementLibraryFiles& libfiles, StageSink sink,
                    GenerationOptions options = {}, GenerationResources* resources = nullptr)
        : cs_(cs), fs_(fs), classes_(classes), libfiles_(libfiles),
          owned_resources_(resources ? nullptr : std::make_unique<GenerationResources>(fs)),
          resources_(resources ? *resources : *owned_resources_), sink_(std::move(sink)), rng_(cs.rng_state),
          props_(resources_.props), options_(options) {
        if (!resources_.belongs_to(fs)) throw std::invalid_argument("generation resources belong to another installation");
        rng_.record_history = options.record_rng;
    }
    GenerationState(const GenerationState&) = delete;
    GenerationState& operator=(const GenerationState&) = delete;
    GenerationState(GenerationState&&) = delete;
    GenerationState& operator=(GenerationState&&) = delete;

    const CaseSettings& cs_;
    const GameFS& fs_;
    rmdata::ClassData& classes_;
    const rmdata::SettlementLibraryFiles& libfiles_;
    std::unique_ptr<GenerationResources> owned_resources_;
    GenerationResources& resources_;
    StageSink sink_;
    ObjectObserver object_observer_;
    DrawLog rng_;
    std::vector<int> races_, team_, assignment_;
    rmdata::MapTemplate tpl_;
    Grid<std::uint8_t> semantic_, labels_, zone_, cls_;
    Markers markers_;
    int scale_ = 0;
    Library lib_;
    std::map<int, Slot> slots_;
    Grid<float> a_;
    Grid<std::uint16_t> decor_;
    std::vector<Segment> segs_;
    Influence inf_;
    rmdata::ClassProps& props_;
    LibraryObjects libobj_;
    std::vector<MapObject> objs_;
    std::vector<Settlement> setts_;
    Grid<std::uint8_t> h_, light_;
    Grid<float> b_;
    std::string class_attrs_json_;
    std::optional<ClassAttrs> class_attrs_table_;
    const ClassAttrs* class_attrs_ref_ = nullptr;
    GenerationOptions options_;
    std::size_t emitted_bytes = 0;
    std::size_t emitted_stages = 0;

    std::uint32_t seed_a = 0;
    std::vector<int> active;
    bool empty_template = false;

    static std::string lower(std::string s) { return rmdata::lower(std::move(s)); }
    static float bits_float(std::uint32_t bits) {
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    std::vector<std::uint8_t> objects_text() const {
        std::string t;
        auto name = [](std::string s) {
            for (auto& c : s)
                if (c == ' ') c = '_';
            return s;
        };
        static const char* const kinds[] = {"member", "extra", "group", "area"};
        for (const auto& o : objs_) {
            t += std::string(kinds[static_cast<int>(o.kind)]) + ' ' + name(o.cls) + ' ' + std::to_string(o.x) + ' ' + std::to_string(o.y) + ' ';
            t += o.has_facing ? std::to_string(o.f1) + ',' + std::to_string(o.f2) : std::string("-");
            t += ' ' + std::to_string(o.owner) + ' ' + std::to_string(o.settlement) + ' ' + std::to_string(o.partner) + '\n';
        }
        for (const auto& s : setts_) {
            t += "settlement " + (s.record ? "record " + std::to_string(s.idx) : "class " + name(s.cls)) + ' ' + std::to_string(s.owner) + ' ' +
                 std::to_string(s.x) + ' ' + std::to_string(s.y) + ' ' + (s.adjust ? "1" : "0") + ' ' + std::to_string(s.pop_bonus) + '\n';
        }
        return std::vector<std::uint8_t>(t.begin(), t.end());
    }
    bool wants(const std::string& name) const {
        if (!sink_ || options_.stages == StageSelection::None) return false;
        return options_.stages == StageSelection::All || name.rfind("final_", 0) == 0 || name == "map_obj" || name == "objects";
    }
    template<class Producer>
    void emit_lazy(const std::string& name, const std::string& ext, Producer&& produce) {
        if (!wants(name)) return;
        const auto bytes = produce();
        emitted_bytes += bytes.size();
        ++emitted_stages;
        sink_(name, ext, bytes, rng_.draws);
    }
};
}  // namespace iberivm::rm
