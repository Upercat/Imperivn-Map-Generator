// W2 pre-session draws: the primary random state at the start of the map generation, from the syncseed.
//
// Between loading the syncseed and generating the map, session initialization walks slots 0..15 in order. A slot
// below 8 whose race is random (8, or 9 outside the editor) draws its race over the allowed-race mask; then a slot
// whose `real` flag is set and whose name is empty draws a name index Rnd(0, N-1) from its race's table until the
// index is unused for that race (N = `<Race>Names` in CONST.INI, the names `<letter>Name<NNN>`).
//
// A replay stores the settings before these draws, so the state follows by replaying them (`replay_pre_session`).
// A saved game stores them after: races resolved and AI names filled in. `save_pre_session_candidates` searches the
// hypotheses (each slot's race was fixed or random) that reproduce the saved races and names; the name index each
// generated name implies is a strong check. More than one surviving state is resolved by the grids (W5).
#pragma once
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <vector>

#include "dmo_replay.hpp"
#include "game_rng.hpp"

namespace mapx {

// Per-race name tables, race order 0..7 (Gaul, Republican Rome, Carthage, Iberia, Imperial Rome, Britain, Egypt,
// Germany); the key letter is the game's own table G R C I M B E T.
struct NameTables {
    std::array<std::int32_t, 8> counts{};
    std::array<std::vector<std::string>, 8> names;  // names[race][index]; empty string where the key is missing
};

inline constexpr std::array<char, 8> kNameLetters{'G', 'R', 'C', 'I', 'M', 'B', 'E', 'T'};
inline constexpr std::array<const char*, 8> kNameCountKeys{"GaelicNames", "RepublicRomanNames", "CarthageNames",
    "IberianNames", "ImperialRomanNames", "BritonNames", "EgyptiansNames", "GermanNames"};

inline std::string trim(std::string s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// CONST.INI text (latin-1). Keys are matched without case, as the game's INI reader does.
inline NameTables read_name_tables(const std::string& const_ini) {
    NameTables t;
    const std::regex count_re(R"(^\s*(\w+Names)\s*=\s*(-?\d+))", std::regex::icase);
    const std::regex name_re(R"(^\s*([A-Za-z])Name(\d{3})\s*=(.*)$)");
    std::size_t start = 0;
    while (start < const_ini.size()) {
        auto end = const_ini.find('\n', start);
        if (end == std::string::npos) end = const_ini.size();
        std::string line = const_ini.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF: `$` would not match before '\r'
        start = end + 1;
        std::smatch m;
        if (std::regex_search(line, m, count_re)) {
            for (std::size_t r = 0; r < kNameCountKeys.size(); ++r) {
                std::string key = m[1].str(), want = kNameCountKeys[r];
                if (key.size() == want.size() && std::equal(key.begin(), key.end(), want.begin(),
                        [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); }))
                    t.counts[r] = std::stoi(m[2].str());
            }
        } else if (std::regex_search(line, m, name_re)) {
            const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(m[1].str()[0])));
            for (std::size_t r = 0; r < kNameLetters.size(); ++r) {
                if (kNameLetters[r] != letter) continue;
                const auto index = static_cast<std::size_t>(std::stoi(m[2].str()));
                if (t.names[r].size() <= index) t.names[r].resize(index + 1);
                t.names[r][index] = trim(m[3].str());
            }
        }
    }
    return t;
}

// The saved name of an AI player is the table name plus a translated suffix: "Kanakht (IA)", "Kanakht (AI)".
inline std::string strip_ai_suffix(const std::string& name) {
    if (name.size() < 4 || name.back() != ')') return name;
    const auto open = name.rfind(" (");
    return open == std::string::npos ? name : name.substr(0, open);
}

struct PreSession {
    std::uint32_t session_state = 0;     // primary state when the generation session starts
    // Random-map race bytes after session initialization: the settings' bytes, overwritten for drawn slots
    // (0x005223D7 writes the drawn race to rms+0x30+k); an unused slot with a fixed race keeps its byte, even 255.
    std::array<std::int32_t, 8> races{};
    int draws = 0;                       // advancing draws made
    std::uint8_t random_race_slots = 0;  // bit k: slot k's race was drawn (saved games: the hypothesis)
    std::array<std::int32_t, 16> name_index{};  // replays: the generated name index per slot, -1 for none
};

namespace detail {

using Used = std::array<std::array<bool, 256>, 8>;

// 0x00522336..0x005223D7: Rnd(0, 7) for mask 0, -1 or no low byte, else the r-th set bit of the low byte.
inline std::int32_t draw_race(iberivm::GameRng& rng, std::int32_t allowed, int& draws) {
    const std::uint32_t low = static_cast<std::uint32_t>(allowed) & 0xFFu;
    if (allowed == 0 || allowed == -1 || low == 0) {
        ++draws;
        return rng.next(0, 7);
    }
    std::vector<std::int32_t> bits;
    for (std::int32_t i = 0; i < 8; ++i)
        if (low >> i & 1u) bits.push_back(i);
    if (bits.size() > 1) ++draws;
    return bits[static_cast<std::size_t>(rng.next(0, static_cast<std::int32_t>(bits.size()) - 1))];
}

// 0x005BFF50 (draws at 0x005C0124 and, after 256 tries, 0x005C0158): returns the index, marks it used.
inline std::int32_t draw_name(iberivm::GameRng& rng, std::int32_t count, std::array<bool, 256>& used, int& draws) {
    const std::int32_t hi = count - 1;
    std::int32_t i = 0;
    int tries = 0;
    for (; tries < 256; ++tries) {
        i = rng.next(0, hi);
        if (hi != 0) ++draws;
        if (i >= 0 && i < 256 && !used[static_cast<std::size_t>(i)]) break;
    }
    if (tries == 256) {
        i = rng.next(0, hi);
        if (hi != 0) ++draws;
    }
    if (i >= 0 && i < 256) used[static_cast<std::size_t>(i)] = true;
    return i;
}

inline bool race_is_random(std::int32_t race) { return race == 8 || race == 9; }

}  // namespace detail

// Replays: the settings before session initialization, replayed forward.
inline PreSession replay_pre_session(const iberivm::dmo::GameSettings& s, const NameTables& names) {
    iberivm::GameRng rng(s.sync_seed);
    detail::Used used{};
    PreSession out;
    out.name_index.fill(-1);
    for (int k = 0; k < 8; ++k) out.races[static_cast<std::size_t>(k)] = s.random_map.races[static_cast<std::size_t>(k)];
    for (int k = 0; k < 16; ++k) {
        const auto& p = s.players[static_cast<std::size_t>(k)];
        std::int32_t race = p.race;
        if (k < 8 && detail::race_is_random(race)) {
            race = detail::draw_race(rng, p.allowed_races, out.draws);
            out.random_race_slots |= static_cast<std::uint8_t>(1u << k);
            out.races[static_cast<std::size_t>(k)] = race;
        }
        if (p.real != 0 && p.name.empty()) {
            if (race < 0 || race > 7) throw std::runtime_error("name draw for slot " + std::to_string(k) + " with race " + std::to_string(race));
            out.name_index[static_cast<std::size_t>(k)] =
                detail::draw_name(rng, names.counts[static_cast<std::size_t>(race)], used[static_cast<std::size_t>(race)], out.draws);
        }
    }
    out.session_state = rng.state();
    return out;
}

// Saved games: every hypothesis that reproduces the saved races and generated names, one per distinct state.
inline std::vector<PreSession> save_pre_session_candidates(const iberivm::dmo::GameSettings& s, const NameTables& names) {
    struct Branch {
        iberivm::GameRng rng;
        detail::Used used;
        PreSession ps;
    };
    std::vector<Branch> branches{{iberivm::GameRng(s.sync_seed), detail::Used{}, PreSession{}}};
    for (int k = 0; k < 16; ++k) {
        const auto& p = s.players[static_cast<std::size_t>(k)];
        const std::int32_t race = p.race;
        std::vector<Branch> next;
        for (auto& b : branches) {
            // race step: the saved race was either fixed (no draw) or drawn and equal to it
            std::vector<Branch> options{b};
            if (k < 8 && race >= 0 && race <= 7) {
                Branch drawn = b;
                if (detail::draw_race(drawn.rng, p.allowed_races, drawn.ps.draws) == race &&
                    drawn.rng.state() != b.rng.state()) {
                    drawn.ps.random_race_slots |= static_cast<std::uint8_t>(1u << k);
                    options.push_back(drawn);
                }
            }
            for (auto& o : options) {
                // a saved game holds the race bytes as session initialization left them
                if (k < 8) o.ps.races[static_cast<std::size_t>(k)] = s.random_map.races[static_cast<std::size_t>(k)];
                // name step: a saved name that is this race's table name was generated, and the draw must give it
                const std::string base = strip_ai_suffix(p.name);
                const bool generated = p.real != 0 && race >= 0 && race <= 7 && !base.empty() &&
                    std::find(names.names[static_cast<std::size_t>(race)].begin(), names.names[static_cast<std::size_t>(race)].end(), base) !=
                        names.names[static_cast<std::size_t>(race)].end();
                if (generated) {
                    const auto i = detail::draw_name(o.rng, names.counts[static_cast<std::size_t>(race)], o.used[static_cast<std::size_t>(race)], o.ps.draws);
                    const auto& table = names.names[static_cast<std::size_t>(race)];
                    if (i < 0 || static_cast<std::size_t>(i) >= table.size() || table[static_cast<std::size_t>(i)] != base) continue;
                }
                next.push_back(o);
            }
        }
        branches = std::move(next);
        if (branches.empty()) break;
    }
    std::vector<PreSession> out;
    for (auto& b : branches) {
        b.ps.session_state = b.rng.state();
        if (std::none_of(out.begin(), out.end(), [&](const PreSession& o) { return o.session_state == b.ps.session_state; }))
            out.push_back(b.ps);
    }
    return out;
}

// Lobby convention seen in every captured game start (R01, R05, L02 and Asvr MOD's pair): an unused slot below 8
// (`real` clear) keeps the random race. Names cannot confirm the slots after the last AI name, so this breaks
// most ties; the grids (W5) remain the proof.
inline bool unused_slots_random(const PreSession& p, const iberivm::dmo::GameSettings& s) {
    for (int k = 0; k < 8; ++k)
        if (s.players[static_cast<std::size_t>(k)].real == 0 && !(p.random_race_slots >> k & 1u)) return false;
    return true;
}

// The candidate to try first, when one stands out: the only one, or the only one that follows the convention.
inline std::optional<std::size_t> preferred_candidate(const std::vector<PreSession>& c, const iberivm::dmo::GameSettings& s) {
    if (c.size() == 1) return 0;
    std::optional<std::size_t> pick;
    for (std::size_t i = 0; i < c.size(); ++i) {
        if (!unused_slots_random(c[i], s)) continue;
        if (pick) return std::nullopt;
        pick = i;
    }
    return pick;
}

}  // namespace mapx
