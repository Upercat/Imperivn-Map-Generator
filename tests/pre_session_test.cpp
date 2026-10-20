// W2 property test without game files: random skirmish lobbies are run forward (the replay rule), turned into the
// settings a saved game would hold (resolved races, AI names with the translated suffix), and searched backward.
// The true session state must always be among the candidates, and the preferred candidate must never be wrong.
#include <cstdio>
#include <random>
#include <string>

#include "pre_session.hpp"

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++failures;
        std::printf("FAIL %s\n", what);
    }
}

mapx::NameTables synthetic_tables() {
    std::string ini = "[GamePlay]\r\n";
    for (std::size_t r = 0; r < 8; ++r) {
        ini += std::string(mapx::kNameCountKeys[r]) + " = 8\r\n";
        for (int i = 0; i < 8; ++i) {
            char line[64];
            // two equal names in race 6, like the shipped EName003 and EName004 (Bakenptah)
            const int shown = (r == 6 && i == 4) ? 3 : i;
            std::snprintf(line, sizeof line, "%cName%03d = Name%zu_%d\r\n", mapx::kNameLetters[r], i, r, shown);
            ini += line;
        }
    }
    return mapx::read_name_tables(ini);
}

}  // namespace

int main() {
    const auto names = synthetic_tables();
    check(names.counts[6] == 8 && names.names[6].size() == 8 && names.names[6][4] == "Name6_3", "CONST.INI parsing");
    check(mapx::strip_ai_suffix("Kanakht (IA)") == "Kanakht" && mapx::strip_ai_suffix("Player") == "Player", "AI suffix");

    std::mt19937 gen(20261011);
    const int trials = 20000;
    int unique = 0, preferred_right = 0, no_preference = 0;
    for (int t = 0; t < trials; ++t) {
        iberivm::dmo::GameSettings lobby;
        lobby.sync_seed = gen();
        const int active = 1 + static_cast<int>(gen() % 8);
        for (int k = 0; k < 8; ++k) lobby.random_map.races[static_cast<std::size_t>(k)] = k < active ? 8 : 0xFF;
        for (int k = 0; k < 16; ++k) {
            auto& p = lobby.players[static_cast<std::size_t>(k)];
            p.allowed_races = -1;
            if (k >= 8) {
                p.race = 8, p.real = 0, p.name = "Player " + std::to_string(k + 1);
            } else if (k >= active) {
                p.race = 8, p.real = 0;  // unused slot: random race by the lobby convention
            } else {
                p.real = 1;
                p.race = gen() % 3 == 0 ? static_cast<std::int32_t>(gen() % 8) : 8;
                lobby.random_map.races[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(p.race);
                if (k == 0) p.name = "Player";  // the human keeps the profile name
                if (gen() % 10 == 0) p.allowed_races = static_cast<std::int32_t>(gen() % 255 + 1);  // a restricted mask
            }
        }
        const auto truth = mapx::replay_pre_session(lobby, names);

        auto saved = lobby;
        for (int k = 0; k < 8; ++k) {
            // drawn slots: both the player record and the random-map byte hold the drawn race
            if (truth.random_race_slots >> k & 1u) saved.players[static_cast<std::size_t>(k)].race = truth.races[static_cast<std::size_t>(k)];
            saved.random_map.races[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(truth.races[static_cast<std::size_t>(k)]);
        }
        for (int k = 0; k < 16; ++k) {
            const auto i = truth.name_index[static_cast<std::size_t>(k)];
            if (i >= 0) {
                const auto race = static_cast<std::size_t>(truth.races[static_cast<std::size_t>(k)]);
                saved.players[static_cast<std::size_t>(k)].name = names.names[race][static_cast<std::size_t>(i)] + (t % 2 ? " (IA)" : " (AI)");
            }
        }
        const auto c = mapx::save_pre_session_candidates(saved, names);
        const auto pick = mapx::preferred_candidate(c, saved);
        bool found = false;
        for (const auto& p : c) found = found || p.session_state == truth.session_state;
        if (!found) {
            ++failures;
            if (failures < 5) std::printf("FAIL trial %d: true state %08X not among %zu candidates\n", t, truth.session_state, c.size());
        }
        if (c.size() == 1) ++unique;
        if (!pick) ++no_preference;
        else if (c[*pick].session_state == truth.session_state) ++preferred_right;
        else {
            ++failures;
            if (failures < 5) std::printf("FAIL trial %d: preferred %08X, true %08X\n", t, c[*pick].session_state, truth.session_state);
        }
    }
    std::printf("%d lobbies: %d unique, %d resolved by preference, %d left to the grids, %d failures\n", trials, unique,
                preferred_right, no_preference, failures);
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
