#pragma once
#include "rm_generation_state.hpp"

namespace iberivm::rm::generation {

// Inputs: landscape, library, flags, RNG. Outputs: updated field A, terrain classes, decoration, objects and settlements.
inline void place_objects(GenerationState& ctx) {
    const int size = ctx.cs_.size;
    const int nt = ((size - 1) >> 6) + 1;
    const auto& packed = ctx.inf_.packed;
    Grid<std::uint8_t> zero(ctx.a_.rows, ctx.a_.cols);
    for (std::size_t i = 0; i < ctx.a_.v.size(); ++i) zero.v[i] = ctx.a_.v[i] == 0.0f ? 1 : 0;
    const auto syms = symbols(ctx.cs_.flags);
    auto defined = [&](const char* s) { return std::find(syms.begin(), syms.end(), s) != syms.end(); };
    const auto& decor_reg = ctx.resources_.decor_registry();
    // PlaceTrees.xml is wrapped whole in <ifdef id="trees">: with no_trees (rms+0x5C) no tree rule is read, so
    // neither the tree-noise seed nor any placement draw happens (E14: no draw between macrozones_end and PlaceObjects).
    if (defined("trees")) {
        const auto tseed = static_cast<std::uint32_t>(ctx.rng_(0, 999999));
        const int L = log2_field(nt);
        const int m = std::min(2, L);
        const int cnt = std::min(3, L - m + 1);
        const auto tfield = noise_normalise(noise_generate(nt, set_amps(L - cnt - m + 1, cnt, 0.3), tseed));
        place_trees(ctx.rng_, tfield, ctx.inf_.value, ctx.zone_, ctx.resources_.tree_catalogue(), ctx.decor_);
    }
    ctx.emit_lazy("decor_after_trees", "u16", [&] { return raw_bytes(ctx.decor_.v); });
    int players = 0;
    for (int t : ctx.cs_.teams)
        if (t != -1) ++players;
    auto descs = parse_descriptors(ctx.fs_.read_text("data/RandomMap/PlaceObjects.xml"), size, players, syms, ctx.props_);
    const auto areas = load_area_templates(ctx.lib_, ctx.libobj_, ctx.props_);
    // "game_start": generation run by a game start (skirmish lobby, replay) instead of the editor dialog
    PlaceObjects eng(ctx.rng_, size, std::move(descs), ctx.slots_, ctx.races_, ctx.lib_, ctx.libobj_, areas, packed, std::move(zero), ctx.decor_, ctx.a_, ctx.cls_, ctx.props_, decor_reg,
                     ctx.setts_, ctx.cs_.game_start);
    eng.run();
    ctx.emit_lazy("a_after_objects", "f32", [&] { return raw_bytes(ctx.a_.v); });
    ctx.emit_lazy("decor_after_objects", "u16", [&] { return raw_bytes(ctx.decor_.v); });
    ctx.emit_lazy("cls_after_objects", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    ctx.objs_.insert(ctx.objs_.end(), eng.objects.begin(), eng.objects.end());
    ctx.setts_.insert(ctx.setts_.end(), eng.settlements.begin(), eng.settlements.end());
    ctx.emit_lazy("objects", "txt", [&] { return ctx.objects_text(); });
}

}  // namespace iberivm::rm::generation
