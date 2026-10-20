// Synthetic inputs for the W1 reader: every refusal path and both accepted layouts, without game files.
#include <cstdio>
#include <string>
#include <vector>

#include "input_reader.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Writer {
    Bytes b;
    void u8(std::uint8_t v) { b.push_back(v); }
    void u16(std::uint16_t v) { u8(static_cast<std::uint8_t>(v)), u8(static_cast<std::uint8_t>(v >> 8)); }
    void u32(std::uint32_t v) { for (int i = 0; i < 4; ++i) u8(static_cast<std::uint8_t>(v >> (8 * i))); }
    void str(const std::string& s) {
        u32(static_cast<std::uint32_t>(s.size()));
        b.insert(b.end(), s.begin(), s.end());
    }
};

struct Settings {
    std::string base = "Packs/RandomMap";
    std::uint32_t random = 1;
    std::string tpl = "data/RandomMap/CONTINENT.RM.XML";
    std::uint32_t size = 0x4000;
    std::array<std::uint8_t, 8> teams{0, 1, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    std::array<std::uint8_t, 8> races{3, 7, 0, 0, 0, 0, 0, 0};
    std::uint32_t sync = 0xCD5E76C8;
};

void body(Writer& w, const Settings& s) {
    w.str(s.base);
    w.u32(1);
    w.u32(s.random);
    w.str(s.tpl);
    w.u32(s.size);
    for (auto v : s.teams) w.u8(v);
    for (auto v : s.races) w.u8(v);
    for (int i = 0; i < 12; ++i) w.u32(0);
    for (int k = 0; k < 16; ++k) {
        for (int i = 0; i < 5; ++i) w.u32(0);
        w.str("CHAOTIC");
        for (int i = 0; i < 3; ++i) w.u32(0);
        w.str("Player");
        for (int i = 0; i < 3 + 16; ++i) w.u32(0);
    }
    w.u32(0);  // game data kind
    for (int i = 0; i < 7; ++i) w.str("x");
    for (int i = 0; i < 4 + 8 + 3; ++i) w.u32(0);
    w.u32(0x12345678);  // normal seed
    w.u32(s.sync);
    w.u32(0);  // playdemo
}

Bytes replay(const Settings& s, std::uint32_t build = mapx::kHdBuild) {
    Writer w;
    w.u32(iberivm::dmo::kMagic);
    w.u32(build);
    w.u32(mapx::kGameTypeReplay);
    w.u16(73);
    body(w, s);
    w.u32(0);  // command-state entry count (not read by the input reader)
    return w.b;
}

// PersistSavedGame's layout: magic, build, version, gametype, then the settings body (none for a conquest).
Bytes save(const Settings& s, std::uint32_t game_type = mapx::kGameTypeSingle, std::uint32_t version = mapx::kHdSaveVersion) {
    Writer w;
    w.u32(iberivm::dmo::kMagic);
    w.u32(mapx::kHdBuild);
    w.u32(version);
    w.u32(game_type);
    if (game_type != mapx::kGameTypeConquest) body(w, s);
    return w.b;
}

// One stored (uncompressed) block: the decoder copies a block whose span equals its logical length.
Bytes lzis(const Bytes& logical) {
    Writer w;
    for (char c : std::string("LZIS")) w.u8(static_cast<std::uint8_t>(c));
    w.u32(static_cast<std::uint32_t>(logical.size()));
    w.u32(0x20000);
    w.u8(2);
    w.u8(0);
    w.u32(0x12);
    w.b.insert(w.b.end(), logical.begin(), logical.end());
    return w.b;
}

int failures = 0;

void expect(const char* name, const Bytes& raw, mapx::Kind kind, mapx::Problem problem) {
    const auto in = mapx::read_input_bytes(raw);
    const bool ok = in.kind == kind && in.problem == problem;
    if (!ok) ++failures;
    std::printf("%s %-34s %s %s (%s)\n", ok ? "PASS" : "FAIL", name, mapx::kind_name(in.kind),
                mapx::problem_name(in.problem), in.detail.c_str());
}

}  // namespace

int main() {
    using mapx::Kind;
    using mapx::Problem;
    const Settings s;

    expect("empty file", {}, Kind::Unreadable, Problem::NotGameFile);
    expect("text file", Bytes{'h', 'e', 'l', 'l', 'o'}, Kind::Unreadable, Problem::NotGameFile);
    expect("LZIS header only", Bytes{'L', 'Z', 'I', 'S', 1, 0}, Kind::Unreadable, Problem::Damaged);
    {
        Bytes huge = lzis(replay(s));
        huge[4] = huge[5] = huge[6] = 0xFF, huge[7] = 0x7F;  // declared logical size about 2 GB
        expect("hostile declared size", huge, Kind::Unreadable, Problem::Damaged);
    }
    {
        Bytes cut = lzis(replay(s));
        cut[0x0E] = 0xFF, cut[0x0F] = 0xFF;  // block offset past the end of the file
        expect("block offset past end", cut, Kind::Unreadable, Problem::Damaged);
    }
    {
        Bytes logical = replay(s);
        logical.resize(200);
        expect("truncated settings", lzis(logical), Kind::Unreadable, Problem::Damaged);
    }
    {
        Bytes logical = replay(s);
        logical[0] = 'x';
        expect("no gsxv magic", lzis(logical), Kind::Unreadable, Problem::NotGameFile);
    }

    {
        Bytes raw = lzis(replay(s));
        raw[8] = raw[9] = raw[10] = raw[11] = 0xFF;
        expect("hostile block capacity", raw, Kind::Unreadable, Problem::Damaged);
        raw = lzis(replay(s));
        raw[14] = 0; raw[15] = raw[16] = raw[17] = 0;
        expect("block overlaps index", raw, Kind::Unreadable, Problem::Damaged);
        raw = lzis(replay(s));
        const auto end = static_cast<std::uint32_t>(raw.size());
        for (int n = 0; n < 4; ++n) raw[14 + n] = static_cast<std::uint8_t>(end >> (n * 8));
        expect("empty data block", raw, Kind::Unreadable, Problem::Damaged);
    }

    expect("random-map replay", lzis(replay(s)), Kind::RandomMapReplay, Problem::None);
    expect("random-map save", lzis(save(s)), Kind::RandomMapSave, Problem::None);
    {
        Settings r = s;
        r.races[1] = mapx::kRandomRace;
        expect("replay with a random race", lzis(replay(r)), Kind::RandomMapReplay, Problem::None);
        expect("save with an unresolved race", lzis(save(r)), Kind::Unsupported, Problem::ModdedGame);
    }

    expect("old build", lzis(replay(s, 0x10100)), Kind::Unsupported, Problem::GameVersion);
    expect("save version 102 (1.0.1)", lzis(save(s, mapx::kGameTypeSingle, 102)), Kind::Unsupported, Problem::GameVersion);
    expect("unknown gametype", lzis(save(s, 7)), Kind::Unsupported, Problem::UnknownLayout);
    expect("conquest save (no settings)", lzis(save(s, mapx::kGameTypeConquest)), Kind::Campaign, Problem::None);
    {
        Settings p = s;
        p.random = 0;
        p.base = "scenarios/Balcans";
        expect("predefined map", lzis(save(p)), Kind::PredefinedMap, Problem::None);
        p.base = "adventures/GreatBattles/1_Great_Battles_Zama";
        expect("adventure save", lzis(save(p, mapx::kGameTypeAdventure)), Kind::Campaign, Problem::None);
    }
    {
        Settings m = s;
        m.races[0] = 10;
        expect("race 10", lzis(save(m)), Kind::Unsupported, Problem::ModdedGame);
        m = s;
        m.size = 12345;
        expect("unknown map size", lzis(save(m)), Kind::Unsupported, Problem::UnknownLayout);
        m = s;
        m.tpl = "mods/RandomMap/X.RM.XML";
        expect("template outside data/RandomMap", lzis(save(m)), Kind::Unsupported, Problem::UnknownLayout);
        m = s;
        m.teams.fill(0xFF);
        expect("no active slot", lzis(save(m)), Kind::Unsupported, Problem::UnknownLayout);
    }

    const auto in = mapx::read_input_bytes(lzis(save(s)));
    if (in.settings.sync_seed != s.sync || mapx::template_name(in.settings.random_map.template_path) != "CONTINENT") {
        ++failures;
        std::printf("FAIL settings fields\n");
    }
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
