// Browsable random-map batches. Each card has its own seed and produces its final export once.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "generate.hpp"
#include "map_assembly.hpp"
#include "minimap.hpp"

namespace mapx {

struct GalleryTemplate {
    std::string path, name;
    std::array<int, 3> max_players{};  // small, medium, large; zero means unavailable
};

inline std::vector<GalleryTemplate> gallery_templates(const iberivm::GameFS& fs) {
    std::vector<GalleryTemplate> out;
    for (const auto& path : fs.list_files("data/RandomMap")) {
        const auto lower = iberivm::rmdata::lower(path);
        if (!lower.ends_with(".rm.xml") || lower.ends_with("empty.rm.xml")) continue;
        try {
            const auto xml = iberivm::xml::parse(fs.read_text(path));
            if (xml.tag != "map-template" || !xml.child("bitmap")) continue;
            // Match the generator's bitmap reader before advertising a template. Every candidate
            // must be readable, because the seeded shuffle may choose any of them.
            bool has_bitmap = false;
            for (const auto& bitmap : fs.list_files(xml.child("bitmap")->get("file"))) {
                if (!iberivm::rmdata::lower(bitmap).ends_with(".bmp")) continue;
                (void)iberivm::rmdata::decode_bmp16(fs.read(bitmap));
                has_bitmap = true;
            }
            if (!has_bitmap) continue;
            GalleryTemplate item{path, xml.get("name", template_name(path)), {}};
            for (const auto* limit : xml.all("limit")) {
                const auto size = limit->get("map-size");
                const int index = size == "small" ? 0 : size == "medium" ? 1 : size == "large" ? 2 : -1;
                if (index >= 0) item.max_players[static_cast<std::size_t>(index)] =
                    std::clamp(static_cast<int>(iberivm::rmdata::py_int(limit->get("max-players", "0"))), 0, 8);
            }
            if (*std::max_element(item.max_players.begin(), item.max_players.end()) >= 2) out.push_back(std::move(item));
        } catch (const std::exception&) {
            // A malformed/custom template does not prevent the remaining catalogue from being browsed.
        }
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

inline std::optional<std::uint32_t> gallery_seed(const std::string& text) {
    const auto value = trim(text);
    const std::size_t start = value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X') ? 2 : 0;
    if (value.size() <= start || value.size() - start > 8) return std::nullopt;
    std::uint32_t out = 0;
    for (std::size_t i = start; i < value.size(); ++i) {
        const char c = value[i];
        const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                          c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (digit < 0) return std::nullopt;
        out = (out << 4) | static_cast<std::uint32_t>(digit);
    }
    return out;
}

struct GalleryRequest {
    GalleryTemplate map_template;
    int size_index = 1;
    int players = 4;
    int race = 8;  // 0..7 fixed for active players; 8 random
    std::uint32_t base_seed = 0;
};

inline std::uint32_t gallery_card_seed(std::uint32_t base, std::size_t index) {
    if (index >= 9) throw std::out_of_range("gallery card index");
    return base + static_cast<std::uint32_t>(index);  // defined unsigned wrap, still nine distinct seeds
}

inline InputFile gallery_input(const GalleryRequest& request, std::uint32_t seed) {
    if (request.size_index < 0 || request.size_index > 2 || request.players < 2 || request.players > 8 ||
        request.race < 0 || request.race > 8 || request.map_template.path.empty() ||
        request.players > request.map_template.max_players[static_cast<std::size_t>(request.size_index)])
        throw std::invalid_argument("unsupported gallery settings");
    InputFile in;
    in.kind = Kind::RandomMapReplay;  // use the same settings-to-session path, without reading a file
    in.build = kHdBuild;
    in.has_settings = true;
    auto& s = in.settings;
    s.random = 1;
    s.sync_seed = seed;
    s.random_map.template_path = request.map_template.path;
    s.random_map.map_size = 8192u << request.size_index;
    s.random_map.teams.fill(kNoTeam);
    s.random_map.races.fill(kRandomRace);
    for (std::size_t k = 0; k < s.players.size(); ++k) {
        auto& p = s.players[k];
        const bool active = k < static_cast<std::size_t>(request.players);
        p.race = k < 8 ? kRandomRace : 0;
        p.real = active ? 1 : 0;
        p.control = k == 0 ? 0 : 1;
        p.name = active ? "Player " + std::to_string(k + 1) : "";
        if (active) {
            p.race = request.race;
            s.random_map.races[k] = static_cast<std::uint8_t>(request.race);
            s.random_map.teams[k] = static_cast<std::uint8_t>(k);  // free-for-all
        }
    }
    return in;
}

struct GalleryMap {
    InputFile input;
    PreSession pre;
    std::vector<std::uint8_t> volume;
    Image minimap;
    std::array<int, static_cast<std::size_t>(Feature::Count)> counts{};
};

inline std::optional<GalleryMap> generate_gallery_map(Install& install, const GalleryRequest& request, std::size_t index,
                                                     const std::string& language, int pixels,
                                                     const std::function<bool()>& cancelled = {}) {
    if (pixels < 32 || pixels > 1024) throw std::invalid_argument("invalid gallery thumbnail size");
    GalleryMap out;
    out.input = gallery_input(request, gallery_card_seed(request.base_seed, index));
    out.pre = replay_pre_session(out.input.settings, install.names());
    auto map = generate_map_candidate(install, out.input, out.pre, {}, cancelled);
    if (!map || (cancelled && cancelled())) return std::nullopt;
    const auto markers = map_markers(install, *map);
    out.counts = feature_counts(markers);
    out.minimap = render_minimap(install, *map, out.input, pixels, &markers);
    if (cancelled && cancelled()) return std::nullopt;
    out.volume = pack_map(install, assemble_map(install, *map, out.input, out.pre, language));
    return out;
}

}  // namespace mapx
