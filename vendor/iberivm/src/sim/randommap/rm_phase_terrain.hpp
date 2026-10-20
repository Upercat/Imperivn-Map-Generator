#pragma once
#include "rm_generation_state.hpp"

namespace iberivm::rm::generation {

// Inputs: field A, anchors, segments, RNG. Outputs: heights, mountain terrain classes and lighting.
inline void build_heights(GenerationState& ctx) {
    const int size = ctx.cs_.size;
    box_inplace(ctx.a_, 2);  // 0x005823D6..0x005824A1: in-place 5x5 mean (P05)
    ctx.emit_lazy("a_after_blur5", "f32", [&] { return raw_bytes(ctx.a_.v); });
    ctx.h_ = materialize_heights(ctx.a_);
    ctx.emit_lazy("heights_materialized", "u8", [&] { return raw_bytes(ctx.h_.v); });
    ctx.emit_lazy("light_first", "u8", [&] { return raw_bytes(light_native(ctx.h_).v); });
    const auto snap = ctx.h_;
    const long long rect[4] = {0, 0, size - 1, size - 1};
    for (const auto& s : ctx.segs_) {
        bool skip = false;
        for (int k = 0; k < 8 && !skip; ++k) {
            const auto it = ctx.slots_.find(k);
            if (it == ctx.slots_.end()) continue;
            for (const auto& anc : {it->second.primary, it->second.village})
                for (const auto& [px, py] : {std::make_pair(s.x0, s.y0), std::make_pair(s.x1, s.y1)}) {
                    const long long dx = px - anc.first, dy = py - anc.second;
                    if (isqrt((dx * dx + dy * dy) & 0xFFFFFFFFll) < 0x900) skip = true;
                }
        }
        if (!skip) mountain_segment(ctx.h_, rect, {s.x0, s.y0}, 0xFF, {s.x1, s.y1}, 0xFF, ctx.rng_);
    }
    ctx.emit_lazy("heights_after_mountains", "u8", [&] { return raw_bytes(ctx.h_.v); });
    for (int r = 0; r < ctx.cls_.rows; ++r)
        for (int c = 0; c < ctx.cls_.cols; ++c)
            if (snap.at(2 * r, 2 * c) != ctx.h_.at(2 * r, 2 * c) || snap.at(2 * r + 1, 2 * c + 1) != ctx.h_.at(2 * r + 1, 2 * c + 1))
                ctx.cls_.at(r, c) = 6;
    ctx.emit_lazy("cls_after_mountains", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    ctx.light_ = light_native(ctx.h_);
    ctx.emit_lazy("light_after_mountains", "u8", [&] { return raw_bytes(ctx.light_.v); });
}

// Inputs: field A, heights, macrozones, terrain tables, RNG. Outputs: field B, final terrain classes and smoothed field A.
inline void paint_terrain(GenerationState& ctx) {
    const int n = ctx.a_.rows;
    const auto seed_b = static_cast<std::uint32_t>(ctx.rng_(0, 0xFFFE));
    const int L = log2_field(n);
    auto b = noise_normalise(noise_generate(n, set_amps(L - 4, 4, 0.75), seed_b));
    const double ca = static_cast<double>(ctx.bits_float(0x3E4CCCCCu)), cb = static_cast<double>(ctx.bits_float(0x3F4CCCCDu));
    for (std::size_t i = 0; i < b.v.size(); ++i)
        b.v[i] = f32(static_cast<double>(ctx.a_.v[i]) * ca + static_cast<double>(b.v[i]) * cb);
    b = noise_normalise(b);
    ctx.emit_lazy("b_final", "f32", [&] { return raw_bytes(b.v); });
    box_inplace(ctx.a_, 3);  // 0x00582719..0x005827F6: A in-place 7x7 mean
    ctx.emit_lazy("a_after_blur7", "f32", [&] { return raw_bytes(ctx.a_.v); });
    ctx.emit_lazy("cls_before_quantise", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    // Quantise 0x0057CCA0(A, 13, 1, 0.0, 0.05f, 1)
    const double hi = f32(0.05);
    for (int r = 0; r < n; ++r)
        for (int c = 0; c < n; ++c) {
            if (ctx.cls_.at(r >> 1, c >> 1) != 0x0F) continue;
            const double v = ctx.a_.at(r, c);
            if (!(0.0 <= v && v <= hi)) continue;
            if (c > 0 && ctx.h_.at(r, c - 1) != ctx.h_.at(r, c)) continue;
            if (r > 0 && ctx.h_.at(r - 1, c) != ctx.h_.at(r, c)) continue;
            ctx.cls_.at(r >> 1, c >> 1) = 13;
        }
    ctx.emit_lazy("cls_after_quantise", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    // Fill 0x0057E7B0(B): zone group, v = trunc(B[2r,2c]*1000.0f)
    const auto& tables = ctx.resources_.terrain_tables();
    const int nc = n / 2;
    for (int r = 0; r < nc; ++r)
        for (int c = 0; c < nc; ++c) {
            if (ctx.cls_.at(r, c) != 0x0F) continue;
            const int z = ctx.zone_.at((r * 64) >> 7, (c * 64) >> 7) & 3;
            const auto v = static_cast<long long>(std::trunc(static_cast<double>(b.at(2 * r, 2 * c)) * 1000.0));
            ctx.cls_.at(r, c) = static_cast<std::uint8_t>(mix_select(tables[static_cast<std::size_t>(z)], v));
        }
    ctx.emit_lazy("cls_after_fill", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    ctx.cls_ = erode_native(ctx.cls_, 5, 13, 1);
    ctx.emit_lazy("cls_after_erode1", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    ctx.cls_ = erode_native(ctx.cls_, 12, 13, 2);
    ctx.emit_lazy("cls_after_erode2", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    static const int tr[6][5] = {{9, 0x10, 3, 4, 0x12}, {9, 0x11, 0, 1, 2}, {1, 0, 3, 4, 0x12},
                                 {1, 2, 9, 0x10, 0x11}, {3, 0, 1, 2, 0x11}, {4, 0, 1, 2, 0x11}};
    for (int k = 0; k < 6; ++k) {
        transition_native(ctx.cls_, tr[k][0], tr[k][1], &tr[k][2]);
        ctx.emit_lazy(k < 5 ? "cls_after_tr" + std::to_string(k + 1) : std::string("cls_after_transitions"), "u8", [&] { return raw_bytes(ctx.cls_.v); });
    }
    ctx.b_ = std::move(b);
}

// Inputs: terrain, objects, library, flags, RNG. Outputs: terrain and objects after shipyards.
inline void place_shipyards(GenerationState& ctx) {
    const auto it = ctx.cs_.flags.find(0x48);
    if (it == ctx.cs_.flags.end() || it->second == 0) {
        auto& sd = ctx.resources_.shipyard_data(ctx.classes_);
        ShipyardContext st{ctx.cs_.size, ctx.cls_, ctx.h_, ctx.decor_, ctx.objs_, ctx.setts_, ctx.lib_, ctx.libobj_, ctx.props_, ctx.rng_, {}};
        for (const auto& [k, s] : ctx.slots_) st.by_race[ctx.races_[static_cast<std::size_t>(k)]].push_back(s.primary);
        iberivm::rm::place_shipyards(st, sd);
    }
    ctx.emit_lazy("heights_after_shipyards", "u8", [&] { return raw_bytes(ctx.h_.v); });
    ctx.emit_lazy("cls_after_shipyards", "u8", [&] { return raw_bytes(ctx.cls_.v); });
    ctx.emit_lazy("decor_after_shipyards", "u16", [&] { return raw_bytes(ctx.decor_.v); });
    ctx.emit_lazy("light_after_shipyards", "u8", [&] { return raw_bytes(ctx.light_.v); });
}

}  // namespace iberivm::rm::generation
