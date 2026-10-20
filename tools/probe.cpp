// Internal console probe for the tool's pipeline (never shipped).
//   map-extractor-probe <file>...                 one line per file
//   map-extractor-probe --check <list> <base_dir>  compare with the expected classification in <list>
// List lines: `<kind> <problem> <path relative to base_dir>`; `#` starts a comment; missing files are skipped.
#include <chrono>
#include <cstdio>
#include <iostream>
#include <sstream>
#include <string>

#include "game_fs.hpp"
#include "sha256.hpp"
#include "generate.hpp"
#include "extract.hpp"
#include "map_assembly.hpp"
#include "minimap.hpp"
#include "input_reader.hpp"
#include "pre_session.hpp"

namespace {

std::filesystem::path utf8_path(const std::string& s) {
    return std::filesystem::path(std::u8string(s.begin(), s.end()));
}

mapx::NameTables name_tables(const std::string& game_root) {
    const iberivm::GameFS fs(std::filesystem::path(std::u8string(game_root.begin(), game_root.end())));
    return mapx::read_name_tables(fs.read_text("DATA/CONST.INI"));
}

// Session-begin candidates of a random-map input: one for a replay, zero or more for a saved game.
std::vector<mapx::PreSession> pre_session(const mapx::InputFile& in, const mapx::NameTables& names) {
    if (in.kind == mapx::Kind::RandomMapReplay) return {mapx::replay_pre_session(in.settings, names)};
    if (in.kind == mapx::Kind::RandomMapSave) return mapx::save_pre_session_candidates(in.settings, names);
    return {};
}

// `*` marks the preferred candidate (mapx::preferred_candidate).
std::string describe_pre(const std::vector<mapx::PreSession>& c, std::optional<std::size_t> pick) {
    std::string out = std::to_string(c.size()) + " candidate(s)";
    char buf[64];
    for (std::size_t i = 0; i < c.size(); ++i) {
        std::snprintf(buf, sizeof buf, " [%s%08X draws=%d random=%02X]", pick == i ? "*" : "", c[i].session_state, c[i].draws,
                      c[i].random_race_slots);
        out += buf;
    }
    return out;
}

// List lines: `<verification> <path>`: the whole extraction's verdict for each input.
int extract_check(const std::string& list_path, const std::string& base_dir, const std::string& game_root) {
    mapx::Install install{utf8_path(game_root)};
    std::ifstream list(utf8_path(list_path), std::ios::binary);
    int passed = 0, failed = 0, skipped = 0;
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string want, rel;
        fields >> want;
        std::getline(fields >> std::ws, rel);
        const auto path = utf8_path(base_dir) / utf8_path(rel);
        if (!std::filesystem::exists(path)) {
            ++skipped;
            std::cout << "SKIP " << rel << '\n';
            continue;
        }
        std::string got;
        try {
            const auto ex = mapx::extract_map(install, mapx::read_input(path));
            got = mapx::verification_name(ex.verification);
            got += " (candidate " + std::to_string(ex.tried) + "/" + std::to_string(ex.candidates) + ")";
        } catch (const mapx::InstallError& e) {
            got = "refused (" + std::string(e.what()) + ")";
        }
        const bool ok = got.rfind(want, 0) == 0;
        (ok ? passed : failed)++;
        std::cout << (ok ? "PASS " : "FAIL ") << got << " | " << rel << '\n';
    }
    std::cout << passed << " passed, " << failed << " failed, " << skipped << " skipped\n";
    return failed ? 1 : 0;
}

// List lines: `<session state hex> <draws> <path>`. A saved game passes when the state is among its candidates;
// more than one candidate is reported as ambiguous (resolved by the grids in W5).
int pre_check(const std::string& list_path, const std::string& base_dir, const std::string& game_root) {
    const auto names = name_tables(game_root);
    std::ifstream list(std::filesystem::path(std::u8string(list_path.begin(), list_path.end())), std::ios::binary);
    int passed = 0, failed = 0, skipped = 0, ambiguous = 0;
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string state_text;
        int draws = 0;
        fields >> state_text >> draws;
        std::string rel;
        std::getline(fields >> std::ws, rel);
        const auto path = std::filesystem::path(std::u8string(base_dir.begin(), base_dir.end())) / std::filesystem::path(std::u8string(rel.begin(), rel.end()));
        if (!std::filesystem::exists(path)) {
            ++skipped;
            std::cout << "SKIP " << rel << '\n';
            continue;
        }
        const auto state = static_cast<std::uint32_t>(std::stoul(state_text, nullptr, 16));
        const auto in = mapx::read_input(path);
        const auto c = pre_session(in, names);
        const auto pick = mapx::preferred_candidate(c, in.settings);
        auto expected = [&](const mapx::PreSession& p) { return p.session_state == state && p.draws == draws; };
        const bool found = std::any_of(c.begin(), c.end(), expected);
        // pass: the preferred candidate is the expected one; ambiguous: no preference, expected among the candidates
        const char* verdict = !found ? "FAIL " : !pick ? "AMBG " : expected(c[*pick]) ? "PASS " : "FAIL ";
        if (verdict[0] == 'F') ++failed;
        else if (verdict[0] == 'A') ++ambiguous, ++passed;
        else ++passed;
        std::cout << verdict << describe_pre(c, pick) << " | " << rel << '\n';
    }
    std::cout << passed << " passed (" << ambiguous << " ambiguous), " << failed << " failed, " << skipped << " skipped\n";
    return failed ? 1 : 0;
}


// Digest of a generated map: SHA-256 over the lines "<sha256 hex of each file>\n" in file-name order, the same as
// `sha256sum` of each Maps/1 file piped into `sha256sum`.
std::string map_digest(const mapx::MapFiles& map) {
    std::string lines;
    for (const auto& [name, bytes] : map.files) lines += iberivm::sha256_hex(bytes) + "\n";
    return iberivm::sha256_hex(std::vector<std::uint8_t>(lines.begin(), lines.end()));
}

// List lines: `<map digest> <volume sha256> <path>`: the input generated with its preferred candidate must give the
// digest of the Python reference's map files, and its packed .BFHP (English texts) the reference volume's SHA-256.
int gen_check(const std::string& list_path, const std::string& base_dir, const std::string& game_root) {
    mapx::Install install{utf8_path(game_root)};
    std::ifstream list(utf8_path(list_path), std::ios::binary);
    int passed = 0, failed = 0, skipped = 0;
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string digest, volume_sha, rel;
        fields >> digest >> volume_sha;
        std::getline(fields >> std::ws, rel);
        const auto path = utf8_path(base_dir) / utf8_path(rel);
        if (!std::filesystem::exists(path)) {
            ++skipped;
            std::cout << "SKIP " << rel << '\n';
            continue;
        }
        const auto in = mapx::read_input(path);
        const auto c = pre_session(in, install.names());
        const auto pick = mapx::preferred_candidate(c, in.settings);
        std::string got = "no candidate", got_volume = "no candidate";
        if (pick) {
            const auto map = mapx::generate_map(install, in, c[*pick]);
            got = map_digest(map);
            got_volume = iberivm::sha256_hex(mapx::pack_map(install, mapx::assemble_map(install, map, in, c[*pick], "english")));
        }
        const bool ok = got == digest && got_volume == volume_sha;
        (ok ? passed : failed)++;
        std::cout << (ok ? "PASS " : "FAIL ") << "map " << got.substr(0, 16) << " volume " << got_volume.substr(0, 16) << " | " << rel << '\n';
    }
    std::cout << passed << " passed, " << failed << " failed, " << skipped << " skipped\n";
    return failed ? 1 : 0;
}

std::string describe(const mapx::InputFile& in) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "%-13s %-14s build=%05X version=%u gametype=%u", mapx::kind_name(in.kind),
                  mapx::problem_name(in.problem), in.build, in.version, in.game_type);
    std::string out = buf;
    if (in.has_settings && in.settings.random == 1) {
        const auto& rm = in.settings.random_map;
        std::snprintf(buf, sizeof buf, " size=%u slots=%zu sync=%08X ", rm.map_size, mapx::active_slots(rm).size(),
                      in.settings.sync_seed);
        out += buf + mapx::template_name(rm.template_path);
    }
    if (!in.detail.empty()) out += " (" + in.detail + ")";
    return out;
}

int check(const std::string& list_path, const std::string& base_dir) {
    std::ifstream list(utf8_path(list_path), std::ios::binary);
    if (!list) {
        std::cerr << "cannot open " << list_path << '\n';
        return 2;
    }
    int passed = 0, failed = 0, skipped = 0;
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        std::string kind, problem;
        fields >> kind >> problem;
        std::string rel;
        std::getline(fields >> std::ws, rel);
        const auto path = utf8_path(base_dir) / utf8_path(rel);
        if (!std::filesystem::exists(path)) {
            ++skipped;
            std::cout << "SKIP " << rel << '\n';
            continue;
        }
        const auto in = mapx::read_input(path);
        const bool ok = kind == mapx::kind_name(in.kind) && problem == mapx::problem_name(in.problem);
        (ok ? passed : failed)++;
        std::cout << (ok ? "PASS " : "FAIL ") << describe(in) << " | " << rel << '\n';
        if (!ok) std::cout << "     expected " << kind << ' ' << problem << '\n';
    }
    std::cout << passed << " passed, " << failed << " failed, " << skipped << " skipped\n";
    return failed ? 1 : 0;
}

}  // namespace

// Wide arguments: the narrow argv of a Windows console program is in the ANSI code page and loses accented names.
int run(int argc, wchar_t** argv) {
    auto arg = [&](int i) {
        const auto u8 = std::filesystem::path(argv[i]).u8string();
        return std::string(u8.begin(), u8.end());
    };
    if (argc == 4 && arg(1) == "--check") return check(arg(2), arg(3));
    if (argc == 5 && arg(1) == "--pre-check") return pre_check(arg(2), arg(3), arg(4));
    if (argc == 5 && arg(1) == "--gen-check") return gen_check(arg(2), arg(3), arg(4));
    if (argc == 5 && arg(1) == "--extract-check") return extract_check(arg(2), arg(3), arg(4));
    // --generate <game_root> <file> <out_dir> [candidate index]: writes <out_dir>/Maps/1/<map files>
    if ((argc == 5 || argc == 6) && arg(1) == "--generate") {
        const auto t0 = std::chrono::steady_clock::now();
        mapx::Install install{std::filesystem::path(argv[2])};
        const double load = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto in = mapx::read_input(std::filesystem::path(argv[3]));
        const auto c = pre_session(in, install.names());
        const auto pick = argc == 6 ? std::optional<std::size_t>(std::stoul(arg(5))) : mapx::preferred_candidate(c, in.settings);
        if (!pick || *pick >= c.size()) {
            std::cerr << "no candidate to generate: " << mapx::kind_name(in.kind) << ' ' << describe_pre(c, pick) << '\n';
            return 1;
        }
        const auto map = mapx::generate_map(install, in, c[*pick]);
        const auto dir = std::filesystem::path(argv[4]) / "Maps" / "1";
        std::filesystem::create_directories(dir);
        for (const auto& [name, bytes] : map.files) {
            std::ofstream f(dir / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        }
        std::printf("state %08X, %u draws, %zu files, install %.2f s, generation %.2f s\n", c[*pick].session_state, map.draws,
                    map.files.size(), load, map.seconds);
        return 0;
    }
    // --bfhp <game_root> <file> <out.BFHP> [language]: the playable volume (English template texts by default)
    if ((argc == 5 || argc == 6) && arg(1) == "--bfhp") {
        mapx::Install install{std::filesystem::path(argv[2])};
        const auto in = mapx::read_input(std::filesystem::path(argv[3]));
        const auto c = pre_session(in, install.names());
        const auto pick = mapx::preferred_candidate(c, in.settings);
        if (!pick) {
            std::cerr << "no candidate to generate: " << mapx::kind_name(in.kind) << ' ' << describe_pre(c, pick) << '\n';
            return 1;
        }
        const auto map = mapx::generate_map(install, in, c[*pick]);
        const auto files = mapx::assemble_map(install, map, in, c[*pick], argc == 6 ? arg(5) : "english");
        const auto vol = mapx::pack_map(install, files);
        std::ofstream f(std::filesystem::path(argv[4]), std::ios::binary);
        f.write(reinterpret_cast<const char*>(vol.data()), static_cast<std::streamsize>(vol.size()));
        std::printf("%zu files, %zu bytes, SHA-256 %s\n", files.items().size(), vol.size(), iberivm::sha256_hex(vol).c_str());
        return f ? 0 : 1;
    }
    // --extract <game_root> <file>...: the whole extraction with verification, one line per file
    if (argc >= 4 && arg(1) == "--extract") {
        mapx::Install install{std::filesystem::path(argv[2])};
        for (int i = 3; i < argc; ++i) {
            const auto in = mapx::read_input(std::filesystem::path(argv[i]));
            std::cout << mapx::kind_name(in.kind) << ' ';
            if (in.kind != mapx::Kind::RandomMapReplay && in.kind != mapx::Kind::RandomMapSave) {
                std::cout << mapx::problem_name(in.problem) << " | " << arg(i) << '\n';
                continue;
            }
            try {
                const auto t0 = std::chrono::steady_clock::now();
                const auto ex = mapx::extract_map(install, in);
                std::string grids;
                for (const auto& g : ex.grids) grids += std::string(g.equal ? "=" : "x");
                std::printf("%-14s candidate %zu/%zu state %08X grids %s %.1f s", mapx::verification_name(ex.verification), ex.tried,
                            ex.candidates, ex.pre.session_state, grids.empty() ? "-" : grids.c_str(),
                            std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
            } catch (const mapx::InstallError& e) {
                std::cout << "refused " << static_cast<int>(e.problem) << " (" << e.what() << ")";
            } catch (const std::exception& e) {
                std::cout << "error (" << e.what() << ")";
            }
            std::cout << " | " << arg(i) << std::endl;
        }
        return 0;
    }
    // --scan-draws <game_root> <save> <count>: diagnostic for a save no candidate matches; generates from the syncseed
    // advanced by 0..count-1 draws (any pre-session draw count) and reports which, if any, reproduces its grids.
    if (argc == 5 && arg(1) == "--scan-draws") {
        mapx::Install install{std::filesystem::path(argv[2])};
        const auto in = mapx::read_input(std::filesystem::path(argv[3]));
        const auto saved = mapx::saved_grids(in);
        if (saved.empty()) {
            std::cerr << "no saved grids\n";
            return 1;
        }
        auto base = mapx::save_pre_session_candidates(in.settings, install.names());
        mapx::PreSession p = base.empty() ? mapx::PreSession{} : base.front();
        if (base.empty())
            for (int k = 0; k < 8; ++k) p.races[static_cast<std::size_t>(k)] = in.settings.random_map.races[static_cast<std::size_t>(k)];
        iberivm::GameRng rng(in.settings.sync_seed);
        const int count = std::stoi(arg(4));
        for (int k = 0; k < count; ++k) {
            p.session_state = rng.state();
            p.draws = k;
            const auto checks = mapx::compare_grids(mapx::generate_map(install, in, p), saved);
            std::string grids;
            for (const auto& g : checks) grids += g.equal ? "=" : "x";
            if (mapx::required_equal(checks) || k % 10 == 0) std::printf("draws %2d state %08X grids %s\n", k, p.session_state, grids.c_str());
            rng.next(0, 1);
        }
        return 0;
    }
    // --minimap <game_root> <out_dir> <file>...: one 512-pixel minimap BMP per random-map input, named after it
    if (argc >= 5 && arg(1) == "--minimap") {
        mapx::Install install{std::filesystem::path(argv[2])};
        const std::filesystem::path out_dir(argv[3]);
        std::filesystem::create_directories(out_dir);
        for (int i = 4; i < argc; ++i) {
            const std::filesystem::path file(argv[i]);
            const auto in = mapx::read_input(file);
            try {
                const auto ex = mapx::extract_map(install, in);
                const auto bmp = mapx::to_bmp(mapx::render_minimap(install, ex.map, in, 512));
                std::ofstream f(out_dir / (file.filename().wstring() + L".png.bmp"), std::ios::binary);
                f.write(reinterpret_cast<const char*>(bmp.data()), static_cast<std::streamsize>(bmp.size()));
                std::cout << mapx::verification_name(ex.verification) << " | " << arg(i) << '\n';
            } catch (const std::exception& e) {
                std::cout << "skipped (" << e.what() << ") | " << arg(i) << '\n';
            }
        }
        return 0;
    }
    if (argc >= 4 && arg(1) == "--pre") {
        const auto names = name_tables(arg(2));
        for (int i = 3; i < argc; ++i) {
            const auto in = mapx::read_input(std::filesystem::path(argv[i]));
            const auto c = pre_session(in, names);
            std::cout << mapx::kind_name(in.kind) << ' ' << describe_pre(c, mapx::preferred_candidate(c, in.settings)) << " | " << arg(i) << '\n';
        }
        return 0;
    }
    if (argc < 2 || arg(1).rfind("--", 0) == 0) {
        std::cerr << "usage: map-extractor-probe <file>... | --check <list> <base_dir>\n"
                     "       --pre <game_root> <file>... | --pre-check <list> <base_dir> <game_root>\n"
                     "       --generate <game_root> <file> <out_dir> [candidate] | --gen-check <list> <base_dir> <game_root>\n";
        return 2;
    }
    for (int i = 1; i < argc; ++i) std::cout << describe(mapx::read_input(std::filesystem::path(argv[i]))) << " | " << arg(i) << '\n';
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    try {
        return run(argc, argv);
    } catch (const mapx::InstallError& e) {
        std::cerr << "install problem " << static_cast<int>(e.problem) << ": " << e.what() << '\n';
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << '\n';
    }
    return 3;
}
