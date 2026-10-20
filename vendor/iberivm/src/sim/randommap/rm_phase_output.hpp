#pragma once
#include "rm_generation_state.hpp"

namespace iberivm::rm::generation {

// Inputs: terrain grids, objects, season and class attributes. Outputs: culled objects and emitted final map files.
inline void write_output(GenerationState& ctx) {
    if (ctx.object_observer_) ctx.object_observer_(true, ctx.objs_, ctx.setts_);
    const int size = ctx.cs_.size;
    auto& pd = ctx.resources_.pass_data();
    const auto& decor_reg = ctx.resources_.decor_registry();
    auto rebuild = [&](const std::vector<MapObject>& objects, const std::string& season) {
        Passability pa(size, ctx.h_, ctx.cls_, std::move(ctx.resources_.pass_workspace));
        pa.terrain(pd);
        for (const auto& o : objects)
            if (const auto* fp = pd.footprint(ctx.classes_.class_entity(o.cls, season))) pa.footprint(*fp, o.x, o.y);
        for (int cy = 0; cy < ctx.decor_.rows; ++cy)
            for (int cx = 0; cx < ctx.decor_.cols; ++cx) {
                const int e = ctx.decor_.at(cy, cx);
                if (!e) continue;
                const int q = e >> 8;
                const auto it = decor_reg.entity.find(e & 0xFF);
                const auto* fp = it == decor_reg.entity.end() ? nullptr : pd.footprint(it->second);
                if (fp) pa.footprint(*fp, cx * 64 + 4 * (q & 15), cy * 64 + 4 * (q >> 4));
            }
        pa.borders();
        auto packed = pa.bits.v.empty() ? std::vector<std::uint8_t>{} : pa.packed();
        ctx.resources_.pass_workspace = std::move(pa.bits);
        return packed;
    };
    auto pass = rebuild(ctx.objs_, "spring");
    // the bit grid is needed for the cull: rebuild returns the packed bytes, so unpack the blocked test here
    const int pn = size >> 4;
    auto blocked_at = [&](int x, int y) {
        const int r = y >> 4, c = x >> 4;
        return (pass[static_cast<std::size_t>(r) * (pn / 8) + static_cast<std::size_t>(c / 8)] >> (c % 8)) & 1;
    };
    std::size_t kept = 0;
    for (std::size_t index = 0; index < ctx.objs_.size(); ++index) {
        const auto& o = ctx.objs_[index];
        const bool unit = o.kind == MapObject::Kind::group;  // flags & 0x400000 among the generated objects
        // CPostGenObjCollector::Visit 0x0057F4B0: blocked cell and class initial_z (+0xB44) unset -> removed; otherwise a
        // water_unit (+0xB30) class not standing on terrain class 13 -> removed (field names: Saul P21)
        const bool blocked = unit && blocked_at(o.x, o.y);
        const bool no_z = !unit || !ctx.props_.prop(o.cls, "initial_z");
        bool water_off = false;
        if (unit) {
            const auto wu = ctx.props_.prop(o.cls, "water_unit");
            water_off = (wu && !wu->empty() ? rmdata::py_int(*wu) : 0) != 0 && ctx.cls_.at(o.y >> 6, o.x >> 6) != 13;
        }
        if ((blocked && no_z) || water_off) continue;
        if (kept != index) ctx.objs_[kept] = std::move(ctx.objs_[index]);
        ++kept;
    }
    ctx.objs_.resize(kept);
    if (ctx.object_observer_) ctx.object_observer_(false, ctx.objs_, ctx.setts_);
    // The session generates with spring entities; the chosen season is applied after the session returns and the
    // saved passability uses that season's class entities over the kept objects (E10)
    if (ctx.cs_.season != "spring") pass = rebuild(ctx.objs_, ctx.cs_.season);
    auto grid = [&](std::uint32_t cell, std::uint32_t bits, const std::vector<std::uint8_t>& payload) {
        std::vector<std::uint8_t> out;
        out.reserve(20 + payload.size());
        out.insert(out.end(), {'D', 'I', 'R', 'G'});
        for (std::uint32_t v : {cell, bits, static_cast<std::uint32_t>(size), static_cast<std::uint32_t>(size)})
            for (int k = 0; k < 4; ++k) out.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
        out.insert(out.end(), payload.begin(), payload.end());
        return out;
    };
    ctx.emit_lazy("final_terrain", "bin", [&] { return grid(64, 8, ctx.cls_.v); });
    ctx.emit_lazy("final_height", "bin", [&] { return grid(32, 8, ctx.h_.v); });
    ctx.emit_lazy("final_light", "bin", [&] { return grid(32, 8, ctx.light_.v); });
    ctx.emit_lazy("final_decor", "bin", [&] { return grid(64, 16, raw_bytes(ctx.decor_.v)); });
    ctx.emit_lazy("final_pass", "bin", [&] { return grid(16, 1, pass); });
    ctx.emit_lazy("final_trans", "bin", [&] { return grid(64, 4, std::vector<std::uint8_t>(static_cast<std::size_t>(size >> 6) * (size >> 6) / 2, 0)); });
    if (ctx.class_attrs_json_.empty() && !ctx.class_attrs_table_ && !ctx.class_attrs_ref_)
        throw std::runtime_error("class attribute table not given");
    ctx.emit_lazy("map_obj", "xml", [&] {
        std::optional<ClassAttrs> parsed;
        const ClassAttrs* ca = ctx.class_attrs_ref_;
        if (!ca && ctx.class_attrs_table_) ca = &*ctx.class_attrs_table_;
        if (!ca) { parsed = parse_class_attrs(ctx.class_attrs_json_); ca = &*parsed; }
        MapObjWriter writer(*ca, ctx.props_, ctx.lib_);
        const auto text = writer.write(ctx.objs_, ctx.setts_);
        return std::vector<std::uint8_t>(text.begin(), text.end());
    });
}

}  // namespace iberivm::rm::generation
