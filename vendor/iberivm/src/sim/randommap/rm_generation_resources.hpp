// Installation-scoped, lazy caches. Recreate when installation data changes; not thread safe.
#pragma once
#include <memory>
#include <optional>
#include "randommap_props.hpp"
#include "randommap_rules.hpp"
#include "rm_passability.hpp"
#include "rm_terrain.hpp"
#include "rm_shipyards.hpp"

namespace iberivm::rm {
class GenerationResources {
public:
    explicit GenerationResources(const GameFS& fs) : props(fs), fs_(fs) {}
    GenerationResources(const GenerationResources&) = delete;
    GenerationResources& operator=(const GenerationResources&) = delete;
    rmdata::ClassProps props;
    Grid<std::uint8_t> pass_workspace;  // reused only during sequential output phases
    struct Loads { unsigned decor = 0, trees = 0, terrain = 0, pass = 0, shipyards = 0; } loads;

    bool belongs_to(const GameFS& fs) const { return &fs == &fs_; }
    const rmdata::DecorRegistry& decor_registry() {
        if (!decor_) { decor_ = rmdata::load_decor_registry(fs_); ++loads.decor; }
        return *decor_;
    }
    const std::vector<rmdata::TreeTable>& tree_catalogue() {
        if (!trees_) { trees_ = rmdata::load_tree_catalogue(fs_, decor_registry()); ++loads.trees; }
        return *trees_;
    }
    const std::vector<MixTable>& terrain_tables() {
        if (!terrain_) { terrain_ = native_tables(fs_.read_text("data/RandomMap/TerrainMix.xml")); ++loads.terrain; }
        return *terrain_;
    }
    ShipyardData& shipyard_data(rmdata::ClassData& classes) {
        if (shipyards_ && shipyards_->classes != &classes)
            throw std::invalid_argument("shipyard resources belong to another class table");
        if (!shipyards_) { shipyards_ = load_shipyard_data(fs_, props, classes); ++loads.shipyards; }
        return *shipyards_;
    }
    PassData& pass_data() {
        if (!pass_) { pass_ = std::make_unique<PassData>(fs_); ++loads.pass; }
        return *pass_;
    }
private:
    const GameFS& fs_;
    std::optional<rmdata::DecorRegistry> decor_;
    std::optional<std::vector<rmdata::TreeTable>> trees_;
    std::optional<std::vector<MixTable>> terrain_;
    std::unique_ptr<PassData> pass_;
    std::optional<ShipyardData> shipyards_;
};
}  // namespace iberivm::rm
