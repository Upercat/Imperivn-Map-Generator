#pragma once
#include "rm_generation_state.hpp"

namespace iberivm::rm::generation {

// Inputs: anchors, template, field seed, library, RNG. Outputs: macrozones, field A, settlement objects, decoration, segments and influence.
inline void build_landscape(GenerationState& ctx) {
    const int size = ctx.cs_.size;
    const int nt = ((size - 1) >> 6) + 1;
    const auto& active = ctx.active;
    const auto seed_a = ctx.seed_a;
    const auto mz0 = static_cast<std::uint32_t>(ctx.rng_(0, 0xFFF));
    const auto mz1 = static_cast<std::uint32_t>(ctx.rng_(0, 0xFFF));
    std::map<int, std::vector<std::pair<int, int>>> by_race;
    for (int k : active) by_race[ctx.races_[static_cast<std::size_t>(k)]].push_back(ctx.slots_[k].primary);
    ctx.zone_ = macrozones(size, by_race, {mz0, mz1});
    ctx.emit_lazy("macrozones", "u8", [&] { return raw_bytes(ctx.zone_.v); });
    const auto tf = template_field(ctx.semantic_, size);
    ctx.emit_lazy("template_field", "f32", [&] { return raw_bytes(tf.v); });
    ctx.a_ = field_a(seed_a, tf);
    ctx.emit_lazy("a_field_a", "f32", [&] { return raw_bytes(ctx.a_.v); });
    ctx.decor_ = Grid<std::uint16_t>(nt, nt, 0);
    ctx.cls_ = Grid<std::uint8_t>(nt, nt, 0x0F);  // context init 0x00587D3C: every class byte unset
    for (int k : active) {
        const auto& sl = ctx.slots_[k];
        corridor(ctx.a_, sl.primary.first, sl.primary.second, sl.village.first, sl.village.second, 0.1, 4);
        spawn_decor(ctx.decor_, ctx.lib_, sl.village_template, sl.village, size);
        flatten(ctx.a_, ctx.lib_, sl.village_template, sl.village, size, 0.1);
        spawn_decor(ctx.decor_, ctx.lib_, sl.town, sl.primary, size);
        flatten(ctx.a_, ctx.lib_, sl.town, sl.primary, size, 0.1);
        settlement_terrain_copy(ctx.cls_, ctx.lib_, sl.village_template, sl.village, size);
        settlement_terrain_copy(ctx.cls_, ctx.lib_, sl.town, sl.primary, size);
        // same spawn function 0x0058E3C0 as the PlaceObjects settlements: item-holder start draws on a game start
        DrawLog* start = ctx.cs_.game_start ? &ctx.rng_ : nullptr;
        settlement_objects(ctx.objs_, ctx.setts_, ctx.lib_, ctx.libobj_, sl.village_template, sl.village, size, k, static_cast<int>(ctx.setts_.size()), start, ctx.props_);
        settlement_objects(ctx.objs_, ctx.setts_, ctx.lib_, ctx.libobj_, sl.town, sl.primary, size, k, static_cast<int>(ctx.setts_.size()), start, ctx.props_);
    }
    ctx.emit_lazy("cls_before_influence", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    ctx.emit_lazy("a_before_influence", "f32", [&] { return raw_bytes(ctx.a_.v); });
    std::vector<Segment> segs;
    for (const auto& s : segments(ctx.markers_[static_cast<std::size_t>(ctx.tpl_.meaning("mountain"))], ctx.labels_))
        segs.push_back({s.x0 * ctx.scale_, s.y0 * ctx.scale_, s.x1 * ctx.scale_, s.y1 * ctx.scale_});
    ctx.segs_ = std::move(segs);
    ctx.inf_ = influence_grid(size, ctx.slots_, ctx.lib_, ctx.a_, 0.1, ctx.segs_);
    ctx.emit_lazy("influence", "u32", [&] { return raw_bytes(ctx.inf_.packed); });
}

}  // namespace iberivm::rm::generation
