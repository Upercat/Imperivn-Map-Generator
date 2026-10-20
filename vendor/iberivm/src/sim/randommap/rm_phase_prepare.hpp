#pragma once
#include "rm_generation_state.hpp"

namespace iberivm::rm::generation {

// Inputs: settings, RNG. Outputs: resolved races; optional uniform empty terrain.
inline void initialize(GenerationState& ctx) {
    const int size = ctx.cs_.size;
    const int nt = ((size - 1) >> 6) + 1;
    ctx.races_ = ctx.cs_.races;
    // 0x00587E60: every slot with race 8 (random) becomes Rnd(0, 7) before the pipeline
    for (int k = 0; k < 8 && k < static_cast<int>(ctx.races_.size()); ++k)
        if (ctx.races_[static_cast<std::size_t>(k)] == 8) ctx.races_[static_cast<std::size_t>(k)] = ctx.rng_(0, 7);
    // Template name containing "empty.rm.xml" (test at 0x00581D82): 0x0057CAE0 sets every class byte to 3 and every
    // height byte to 0x40, rebuilds the light grid and the orchestrator returns before the template load; no stage,
    // object or primary draw follows (claim:random-map-empty-template-short-circuits-to-uniform-terrain).
    if (ctx.lower(ctx.cs_.template_path).find("empty.rm.xml") != std::string::npos) {
        const int n = size >> 5;
        ctx.h_ = Grid<std::uint8_t>(n, n, 0x40);
        ctx.light_ = light_native(ctx.h_);
        ctx.cls_ = Grid<std::uint8_t>(nt, nt, 3);
        ctx.decor_ = Grid<std::uint16_t>(nt, nt, 0);
        ctx.empty_template = true;
        return;
    }
}

// Inputs: settings, game files, RNG. Outputs: template, semantic grid, markers, teams and assignments.
inline void load_template(GenerationState& ctx) {
    ctx.tpl_ = rmdata::load_template(ctx.fs_, ctx.cs_.template_name);
    const int seed_x = ctx.rng_(0, 15);
    const auto X = distortion(static_cast<std::uint32_t>(seed_x), ctx.tpl_.amp, true);
    const int seed_y = ctx.rng_(0, 15);
    const auto Y = distortion(static_cast<std::uint32_t>(seed_y), ctx.tpl_.amp, false);
    std::string folder = ctx.tpl_.bitmap;
    for (auto& c : folder)
        if (c == '\\') c = '/';
    while (!folder.empty() && folder.back() == '/') folder.pop_back();
    std::vector<std::string> names;  // std::sort 0x005D1C39 over the upper-case names
    for (const auto& key : ctx.fs_.list_files(folder)) {
        const auto name = key.substr(key.rfind('/') + 1);
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".BMP") == 0) names.push_back(name);
    }
    for (std::size_t i = 1; i < names.size(); ++i) {
        const auto j = static_cast<std::size_t>(ctx.rng_(0, static_cast<std::int32_t>(i)));
        std::swap(names[i], names[j]);
    }
    ctx.team_ = team_vector(ctx.cs_.teams);
    for (const auto& name : names) {
        auto semantic = raster(rmdata::decode_bmp16(ctx.fs_.read(folder + "/" + name)), X, Y, ctx.tpl_.amp, ctx.tpl_.palette);
        int code = 0;
        if (ctx.tpl_.allow_rotate || ctx.tpl_.allow_mirror) {
            code = ctx.rng_(0, 7);
            code &= (ctx.tpl_.allow_rotate ? 6 : 0) | (ctx.tpl_.allow_mirror ? 1 : 0);
            semantic = symmetry(semantic, code);
        }
        auto [vec, labels] = markers(semantic, ctx.cs_.size);
        const auto& strongholds = vec[static_cast<std::size_t>(ctx.tpl_.meaning("player-stronghold"))];
        if (strongholds.size() < ctx.team_.size()) continue;
        StrongholdSearch search(strongholds, labels, ctx.team_, ctx.cs_.size);
        auto asg = search.search(ctx.rng_);
        if (!asg) continue;
        ctx.semantic_ = std::move(semantic);
        ctx.labels_ = std::move(labels);
        ctx.markers_ = std::move(vec);
        ctx.assignment_ = std::move(*asg);
        ctx.emit_lazy("template_semantic", "u8", [&] { return raw_bytes(ctx.semantic_.v); });
        ctx.emit_lazy("template_labels", "u8", [&] { return raw_bytes(ctx.labels_.v); });
        if (ctx.wants("template_choice")) {
            std::vector<std::int32_t> choice{code, seed_x, seed_y, static_cast<std::int32_t>(ctx.assignment_.size())};
            for (int v : ctx.assignment_) choice.push_back(v);
            ctx.emit_lazy("template_choice", "i32", [&] { return raw_bytes(choice); });
        }
        return;
    }
    throw std::runtime_error("INVALIDFORMAT: no candidate accepted");
}

// Inputs: template markers and assignments, races, RNG, library files. Outputs: field seed, active slots, scale, library and anchors.
inline void place_anchors(GenerationState& ctx) {
    const int size = ctx.cs_.size;
    ctx.seed_a = static_cast<std::uint32_t>(ctx.rng_(0, 0xFFFE));
    auto& active = ctx.active;
    for (std::size_t k = 0; k < ctx.team_.size(); ++k)
        if (ctx.team_[k] != -1) active.push_back(static_cast<int>(k));
    // 0x005CE580 (claim:random-map-template-scaling-multiplies-markers-and-clamps-player-markers)
    const int n = size >> 5;
    ctx.scale_ = static_cast<int>(tdiv(n * 32, 512));
    const int hi = (n - 32) * 32;
    auto clampv = [hi](int v) { return v < 0x400 ? 0x400 : (v > hi ? hi : v); };
    const int ps = ctx.tpl_.meaning("player-stronghold");
    std::map<int, std::pair<int, int>> primary;
    for (int k : active) {
        const auto [mx, my] = ctx.markers_[static_cast<std::size_t>(ps)][static_cast<std::size_t>(ctx.assignment_[static_cast<std::size_t>(k)])];
        primary[k] = {clampv(mx * ctx.scale_), clampv(my * ctx.scale_)};
    }
    ctx.lib_.load(ctx.fs_, ctx.classes_, ctx.libfiles_);
    load_library_objects(ctx.libobj_, ctx.lib_);
    ctx.slots_ = anchor_phase(ctx.rng_, size, ctx.races_, active, primary, ctx.lib_.records);
    if (ctx.wants("anchors")) {
        std::vector<std::int32_t> rows;
        for (const auto& [k, s] : ctx.slots_)
            for (int v : {k, s.primary.first, s.primary.second, s.town, s.village_template, s.village.first, s.village.second,
                          s.village_attempts})
                rows.push_back(v);
        ctx.emit_lazy("anchors", "i32", [&] { return raw_bytes(rows); });
    }
}

}  // namespace iberivm::rm::generation
