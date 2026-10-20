// W5 verification and the whole extraction: a saved game's own grids prove the regenerated map; the session-begin
// candidates are tried in order until one matches.
//
// A saved game stores the map grids as DIRG blocks (20-byte header: magic, cell, bits per cell, width, height, then
// the payload) in a fixed order after a 1024-unit 16-bit grid that maps do not have: height, light, transitions,
// terrain classes, passability, decor. Checked on 2026-10-11 against L02, the egipto save and anotherBIG_partida.
// Terrain classes and heights do not change during play; passability does (buildings, units), and decor may.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <stdexcept>
#include <map>
#include <string>
#include <vector>

#include "generate.hpp"
#include "input_reader.hpp"
#include "pre_session.hpp"

namespace mapx {

struct GridSpec {
    const char* file;  // name of the map file (Maps/1)
    std::uint32_t cell, bits;
    bool required;     // must match for the map to count as verified
};
inline constexpr GridSpec kSavedGridOrder[] = {
    {"Terrain.height.grid", 32, 8, true},  {"Terrain.light.grid", 32, 8, false},  {"Terrain.trans.grid", 64, 4, false},
    {"Terrain.terrain.grid", 64, 8, true}, {"Terrain.pass.grid", 16, 1, false},   {"Terrain.decor.grid", 64, 16, false},
};

namespace detail {

inline std::uint32_t le32_at(const std::vector<std::uint8_t>& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
        static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}

// Next DIRG block at or after `from` with this header; returns its offset or npos.
inline std::size_t find_grid(const std::vector<std::uint8_t>& b, std::size_t from, std::uint32_t cell, std::uint32_t bits, std::uint32_t size) {
    static const std::uint8_t magic[] = {'D', 'I', 'R', 'G'};
    for (auto it = std::search(b.begin() + static_cast<std::ptrdiff_t>(from), b.end(), std::begin(magic), std::end(magic)); it != b.end();
         it = std::search(it + 1, b.end(), std::begin(magic), std::end(magic))) {
        const auto at = static_cast<std::size_t>(it - b.begin());
        if (at + 20 > b.size()) break;
        if (le32_at(b, at + 4) == cell && le32_at(b, at + 8) == bits && le32_at(b, at + 12) == size && le32_at(b, at + 16) == size) return at;
    }
    return std::string::npos;
}

}  // namespace detail

// The saved game's map grids by map file name (header included); empty when the sequence is not found.
inline std::map<std::string, std::vector<std::uint8_t>> saved_grids(const InputFile& in) {
    std::map<std::string, std::vector<std::uint8_t>> out;
    const auto size = in.settings.random_map.map_size;
    const auto& b = in.logical;
    std::size_t at = detail::find_grid(b, 0, 1024, 16, size);
    if (at == std::string::npos) return out;
    for (const auto& g : kSavedGridOrder) {
        at = detail::find_grid(b, at + 4, g.cell, g.bits, size);
        if (at == std::string::npos) return {};
        const std::size_t cells = static_cast<std::size_t>(size / g.cell) * (size / g.cell);
        const std::size_t len = 20 + cells * g.bits / 8;
        if (at + len > b.size()) return {};
        out[g.file].assign(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + len));
    }
    return out;
}

enum class Verification {
    Verified,       // saved game: terrain classes and heights equal the regenerated ones
    NotVerifiable,  // replay (no map stored), or a save whose grids were not found
    Mismatch,       // saved game: no candidate reproduces its terrain and heights
};

struct GridCheck {
    std::string file;
    bool required = false, equal = false;
};

struct Extraction {
    Verification verification = Verification::NotVerifiable;
    PreSession pre;                    // the candidate the map was generated from
    std::size_t candidates = 0, tried = 0, early_rejected = 0;
    MapFiles map;
    std::vector<GridCheck> grids;      // saved games: one entry per stored grid
};

inline std::vector<GridCheck> compare_grids(const MapFiles& map, const std::map<std::string, std::vector<std::uint8_t>>& saved) {
    std::vector<GridCheck> out;
    for (const auto& g : kSavedGridOrder) {
        const auto s = saved.find(g.file);
        const auto m = map.files.find(g.file);
        out.push_back({g.file, g.required, s != saved.end() && m != map.files.end() && s->second == m->second});
    }
    return out;
}

inline bool required_equal(const std::vector<GridCheck>& checks) {
    return std::all_of(checks.begin(), checks.end(), [](const GridCheck& c) { return !c.required || c.equal; });
}

// Compare the final unencoded terrain directly with the saved DIRG payloads. Shipyards must be complete.
inline bool required_terrain_equal(const iberivm::rm::GenerationState& state,
                                   const std::map<std::string, std::vector<std::uint8_t>>& saved) {
    auto equal = [&](const char* name, std::uint32_t cell, const std::vector<std::uint8_t>& payload) {
        const auto it = saved.find(name);
        if (it == saved.end()) return false;
        const auto& b = it->second;
        const auto size = static_cast<std::uint32_t>(state.cs_.size);
        return b.size() == 20 + payload.size() && b[0] == 'D' && b[1] == 'I' && b[2] == 'R' && b[3] == 'G' &&
            detail::le32_at(b, 4) == cell && detail::le32_at(b, 8) == 8 && detail::le32_at(b, 12) == size &&
            detail::le32_at(b, 16) == size && std::equal(payload.begin(), payload.end(), b.begin() + 20);
    };
    return equal("Terrain.height.grid", 32, state.h_.v) && equal("Terrain.terrain.grid", 64, state.cls_.v);
}

// No session-begin state reproduces the saved races and names (the game set them some other way).
class NoCandidateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// Progress: called before each generation with (candidate number from 1, candidate count).
using ExtractProgress = std::function<void(std::size_t, std::size_t)>;

// The map of a random-map replay or saved game. Throws InstallError for refusals and NoCandidateError when the
// input has no candidate at all; a save whose grids no candidate matches returns its preferred candidate's map
// with Verification::Mismatch.
inline Extraction extract_map(Install& install, const InputFile& in, const ExtractProgress& progress = {}, bool reject_early = true) {
    if (in.kind != Kind::RandomMapReplay && in.kind != Kind::RandomMapSave) throw std::runtime_error("not a random-map game");
    Extraction ex;
    std::vector<PreSession> candidates;
    if (in.kind == Kind::RandomMapReplay) candidates.push_back(replay_pre_session(in.settings, install.names()));
    else candidates = save_pre_session_candidates(in.settings, install.names());
    if (candidates.empty()) throw NoCandidateError("no session state reproduces the saved races and names");
    // preferred first, the others in search order
    std::vector<std::size_t> order;
    const auto pick = preferred_candidate(candidates, in.settings);
    if (pick) order.push_back(*pick);
    for (std::size_t i = 0; i < candidates.size(); ++i)
        if (!pick || i != *pick) order.push_back(i);
    ex.candidates = candidates.size();

    const auto saved = in.kind == Kind::RandomMapSave ? saved_grids(in) : std::map<std::string, std::vector<std::uint8_t>>{};
    // nothing to verify against: generate the preferred candidate (or the first) only
    if (saved.empty()) order.resize(1);
    std::optional<Extraction> first;
    for (const auto i : order) {
        ++ex.tried;
        if (progress) progress(ex.tried, order.size());
        Extraction e;
        e.candidates = ex.candidates;
        e.tried = ex.tried;
        e.pre = candidates[i];
        // Keep the first complete map for the existing mismatch fallback. Later candidates need no
        // passability or output serialization once their final height/terrain payloads already differ.
        const TerrainAcceptance accept = !saved.empty() && first && reject_early
            ? TerrainAcceptance([&](const auto& state) { return required_terrain_equal(state, saved); }) : TerrainAcceptance{};
        auto generated = generate_map_candidate(install, in, candidates[i], accept);
        if (!generated) { ++ex.early_rejected; continue; }
        e.early_rejected = ex.early_rejected;
        e.map = std::move(*generated);
        if (saved.empty()) {
            e.verification = Verification::NotVerifiable;
            return e;
        }
        e.grids = compare_grids(e.map, saved);
        if (required_equal(e.grids)) {
            e.verification = Verification::Verified;
            return e;
        }
        if (!first) first = std::move(e);
    }
    first->verification = Verification::Mismatch;
    first->tried = ex.tried;
    first->early_rejected = ex.early_rejected;
    return std::move(*first);
}

inline const char* verification_name(Verification v) {
    switch (v) {
    case Verification::Verified: return "verified";
    case Verification::NotVerifiable: return "not-verifiable";
    case Verification::Mismatch: return "mismatch";
    }
    return "?";
}

}  // namespace mapx
