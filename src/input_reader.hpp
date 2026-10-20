// W1 input reader: classifies a replay (.dmo) or saved game (.snr, .adv) and reads its game settings.
//
// Both files are LZIS streams whose logical stream starts with the `gsxv` magic and the build. A replay
// (Game_SaveDemoReplay) then writes gametype 4 and the settings as a polymorphic object (u16 class id, then the body).
// A saved game (PersistSavedGame, read in the HD executable on 2026-10-11) writes a persistence version (103; 102 in
// the 1.0.1 build), then its gametype (1 conquest, 2 adventure, 3 single player or a saveable multiplayer game),
// then, except for a conquest, the settings body without a class id. Checked against 37 distinct saves and 4
// replays (PLAN.md, W1).
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmo_replay.hpp"
#include "lzis.hpp"

namespace mapx {

enum class Kind {
    RandomMapReplay,  // regenerate from the stored settings
    RandomMapSave,    // regenerate, then verify against the stored grids
    PredefinedMap,    // the map is a scenario file: nothing to extract
    Campaign,         // an adventure or conquest save: nothing to extract
    Unsupported,      // readable, but not something this version handles (see Problem)
    Unreadable,       // not a game file, or damaged
};

// Reason shown to the user; the window maps each value to a translated message.
enum class Problem {
    None,
    NotGameFile,    // not LZIS, or no `gsxv` magic
    Damaged,        // truncated or inconsistent stream
    GameVersion,    // a build other than the HD release
    UnknownLayout,  // a gametype or layout not seen yet
    ModdedGame,     // settings outside the unmodified game's ranges
};

inline constexpr std::uint32_t kHdBuild = 0x12800;
inline constexpr std::uint32_t kHdSaveVersion = 103;  // saved games only
inline constexpr std::uint32_t kGameTypeConquest = 1, kGameTypeAdventure = 2, kGameTypeSingle = 3, kGameTypeReplay = 4;
inline constexpr std::uint8_t kNoTeam = 0xFF;   // random-map team byte of an unused slot
inline constexpr std::uint8_t kRandomRace = 8;  // race byte before the session draws

// Sizes guard the decoder against hostile headers; real saves are under 10 MB raw and 64 MB logical.
inline constexpr std::uintmax_t kMaxRawBytes = 128u << 20;
inline constexpr std::uint32_t kMaxLogicalBytes = 512u << 20;

struct InputFile {
    Kind kind = Kind::Unreadable;
    Problem problem = Problem::None;
    std::string detail;  // technical reason for logs, never shown as the user message
    std::uint32_t build = 0;
    std::uint32_t version = 0;    // saved games only
    std::uint32_t game_type = 0;  // kGameType*
    bool has_settings = false;
    iberivm::dmo::GameSettings settings;
    std::size_t settings_end = 0;        // logical offset just after the settings
    std::vector<std::uint8_t> logical;   // kept for the grid verification of saved games (W5)
};

// Active random-map slots: team byte other than 0xFF.
inline std::vector<int> active_slots(const iberivm::dmo::RandomMapSettings& rm) {
    std::vector<int> out;
    for (int k = 0; k < static_cast<int>(rm.teams.size()); ++k)
        if (rm.teams[k] != kNoTeam) out.push_back(k);
    return out;
}

// Template file name without folder and `.RM.XML`, as the game lists it ("CONTINENT").
inline std::string template_name(const std::string& path) {
    std::string name = path.substr(path.find_last_of("/\\") + 1);
    std::string upper = name;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    const std::string suffix = ".RM.XML";
    if (upper.size() > suffix.size() && upper.compare(upper.size() - suffix.size(), suffix.size(), suffix) == 0)
        name.resize(name.size() - suffix.size());
    return name;
}

namespace detail {

inline bool starts_with_ci(const std::string& s, const std::string& prefix) {
    if (s.size() < prefix.size()) return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) return false;
    return true;
}

// The LZIS reader zero-fills blocks that run past the file; a saved game cut short must be reported instead.
inline std::string lzis_layout_problem(const std::vector<std::uint8_t>& raw) {
    // Check size fields before the shared parser allocates an index or output buffer.
    const auto logical_size = iberivm::lzis::le32(raw, 4);
    const auto block_size = iberivm::lzis::le32(raw, 8);
    if (logical_size > kMaxLogicalBytes) return "declared logical size " + std::to_string(logical_size);
    if (block_size > kMaxLogicalBytes) return "declared block size exceeds input limit";
    const auto h = iberivm::lzis::header(raw);
    const auto index_end = 0x0E + 4 * (h.offsets.size() - 1);
    if (h.size != 0 && h.block_size == 0) return "zero block size";
    for (std::size_t i = 0; i + 1 < h.offsets.size(); ++i) {
        if (h.offsets[i] < index_end) return "block overlaps the stream index";
        if (h.size && h.offsets[i] == h.offsets[i + 1]) return "missing block data";
        if (h.offsets[i] > raw.size()) return "block " + std::to_string(i) + " starts past the end of the file";
        if (h.offsets[i + 1] < h.offsets[i]) return "block " + std::to_string(i) + " has a reversed span";
    }
    return {};
}

// The settings body shared by replays and saved games: TVXGameSettings::Serialize after the class id, in the
// order of iberivm::dmo::read_game_settings.
inline iberivm::dmo::GameSettings read_settings_body(iberivm::dmo::StreamReader& r) {
    using namespace iberivm::dmo;
    GameSettings s;
    s.base_map = r.string();
    s.animate_decors = r.u32();
    s.random = r.u32();
    s.random_map = read_random_map_settings(r);
    for (auto& p : s.players) p = read_player_settings(r);
    s.game_data = read_game_data(r);
    for (auto& v : s.basic_flags) v = r.u32();
    s.basic_value_a = r.u32();
    s.basic_value_b = r.u32();
    s.colorize = r.u32();
    s.normal_seed = r.u32();
    s.sync_seed = r.u32();
    s.play_demo = r.u32();
    return s;
}

// Ranges of the unmodified HD game. Anything else is refused rather than generated wrongly.
inline std::string random_map_settings_problem(const iberivm::dmo::RandomMapSettings& rm, bool replay, Problem& problem) {
    problem = Problem::UnknownLayout;
    if (!starts_with_ci(rm.template_path, "data/RandomMap/")) return "template outside data/RandomMap: " + rm.template_path;
    if (rm.map_size != 0x2000 && rm.map_size != 0x4000 && rm.map_size != 0x8000)
        return "map size " + std::to_string(rm.map_size);
    const auto slots = active_slots(rm);
    if (slots.empty()) return "no active slot";
    problem = Problem::ModdedGame;
    for (const int k : slots) {
        if (rm.teams[k] > 7) return "slot " + std::to_string(k) + " team " + std::to_string(rm.teams[k]);
        const std::uint8_t race = rm.races[k];
        if (race > kRandomRace || (race == kRandomRace && !replay))
            return "slot " + std::to_string(k) + " race " + std::to_string(race);
    }
    problem = Problem::None;
    return {};
}

}  // namespace detail

inline InputFile read_input_bytes(const std::vector<std::uint8_t>& raw) {
    InputFile in;
    auto fail = [&](Kind kind, Problem problem, std::string detail) {
        in.kind = kind;
        in.problem = problem;
        in.detail = std::move(detail);
        return in;
    };
    if (!iberivm::lzis::is_lzis(raw)) return fail(Kind::Unreadable, Problem::NotGameFile, "no LZIS signature");
    try {
        if (auto why = detail::lzis_layout_problem(raw); !why.empty()) return fail(Kind::Unreadable, Problem::Damaged, why);
        in.logical = iberivm::lzis::decode(raw);
    } catch (const std::bad_alloc&) {
        return fail(Kind::Unreadable, Problem::Damaged, "LZIS stream too large to decode");
    } catch (const std::exception& e) {
        return fail(Kind::Unreadable, Problem::Damaged, std::string("LZIS: ") + e.what());
    }

    iberivm::dmo::StreamReader r(in.logical);
    try {
        if (r.u32() != iberivm::dmo::kMagic) return fail(Kind::Unreadable, Problem::NotGameFile, "no gsxv magic");
        in.build = r.u32();
        const std::uint32_t third = r.u32();
        if (third == kGameTypeReplay) {
            in.game_type = third;
            in.settings = iberivm::dmo::read_game_settings(r);
        } else {
            in.version = third;
            in.game_type = r.u32();
            // a conquest save may hold its conquest state instead of the game settings: nothing to extract
            if (in.game_type == kGameTypeConquest) return fail(Kind::Campaign, Problem::None, "conquest save");
            if (in.game_type != kGameTypeAdventure && in.game_type != kGameTypeSingle)
                return fail(Kind::Unsupported, Problem::UnknownLayout, "gametype " + std::to_string(in.game_type));
            in.settings = detail::read_settings_body(r);
        }
        in.has_settings = true;
        in.settings_end = r.pos();
    } catch (const std::exception& e) {
        return fail(Kind::Unreadable, Problem::Damaged, std::string("settings: ") + e.what());
    }

    const bool replay = in.game_type == kGameTypeReplay;
    if (in.build != kHdBuild) return fail(Kind::Unsupported, Problem::GameVersion, "build " + std::to_string(in.build));
    if (!replay && in.version != kHdSaveVersion)
        return fail(Kind::Unsupported, Problem::GameVersion, "save version " + std::to_string(in.version));

    if (in.game_type == kGameTypeAdventure || detail::starts_with_ci(in.settings.base_map, "adventures/"))
        return fail(Kind::Campaign, Problem::None, "map " + in.settings.base_map);
    if (in.settings.random == 0) return fail(Kind::PredefinedMap, Problem::None, "map " + in.settings.base_map);
    if (in.settings.random != 1)
        return fail(Kind::Unsupported, Problem::UnknownLayout, "random flag " + std::to_string(in.settings.random));

    Problem problem = Problem::None;
    if (auto why = detail::random_map_settings_problem(in.settings.random_map, replay, problem); !why.empty())
        return fail(Kind::Unsupported, problem, why);
    in.kind = replay ? Kind::RandomMapReplay : Kind::RandomMapSave;
    return in;
}

inline InputFile read_input(const std::filesystem::path& path) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    InputFile in;
    if (ec) {
        in.problem = Problem::NotGameFile;
        in.detail = "cannot read the file size";
        return in;
    }
    if (size > kMaxRawBytes) {
        in.problem = Problem::NotGameFile;
        in.detail = "file larger than any game save";
        return in;
    }
    std::ifstream f(path, std::ios::binary);
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(size));
    if (!f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size()))) {
        in.problem = Problem::NotGameFile;
        in.detail = "cannot read the file";
        return in;
    }
    return read_input_bytes(raw);
}

inline const char* kind_name(Kind k) {
    switch (k) {
    case Kind::RandomMapReplay: return "random-replay";
    case Kind::RandomMapSave: return "random-save";
    case Kind::PredefinedMap: return "predefined";
    case Kind::Campaign: return "campaign";
    case Kind::Unsupported: return "unsupported";
    case Kind::Unreadable: return "unreadable";
    }
    return "?";
}

inline const char* problem_name(Problem p) {
    switch (p) {
    case Problem::None: return "none";
    case Problem::NotGameFile: return "not-game-file";
    case Problem::Damaged: return "damaged";
    case Problem::GameVersion: return "game-version";
    case Problem::UnknownLayout: return "unknown-layout";
    case Problem::ModdedGame: return "modded-game";
    }
    return "?";
}

}  // namespace mapx
