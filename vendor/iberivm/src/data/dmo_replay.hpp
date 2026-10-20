// DMO replay logical stream reader (data module): cerebrivm format:dmo-replay-stream.
//
// The stream is what Game_LoadDemoReplay 0x00596960 reads through CVXPersistStreamLoad (vtable 0x007DC5D0,
// constructor 0x00683AD0) after the LZIS layer (lzis.hpp). The binary load stream writes no field labels;
// its value slots read little-endian fields of fixed width:
//   +0x18 u16 (forwards to +0x3C), +0x1C 8 raw bytes (ReadInt64 0x0067E580), +0x20 n raw bytes (0x0067E510),
//   +0x24 std::string and +0x28/+0x2C char* (u32 length, then the bytes; 0x00681F60, 0x0067ED50),
//   +0x30/+0x34/+0x38 one byte (0x0067E4A0), +0x3C u16 (0x0067E430), +0x40 float (0x0067E3C0),
//   +0x44/+0x48/+0x4C u32 (0x0067E350), +0x50 bool stored as u32 (0x0067E320),
//   +0x54 polymorphic object: u16 class id (0xFFFF null, 0xFFFE back reference + u32 index), then the
//   object's Persist (LoadObject 0x00684D50), +0x58 embedded object: its Persist only (0x0067DB20).
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace iberivm::dmo {

class DmoError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::uint32_t kMagic = 0x76787367;  // "gsxv"

// Sequential little-endian reader over the logical stream with the widths of CVXPersistStreamLoad.
class StreamReader {
public:
    explicit StreamReader(const std::vector<std::uint8_t>& bytes, std::size_t pos = 0) : b_(bytes), p_(pos) {}
    std::size_t pos() const { return p_; }
    std::size_t remaining() const { return b_.size() - p_; }
    std::uint8_t u8() { need(1); return b_[p_++]; }
    std::uint16_t u16() {
        need(2);
        const auto v = static_cast<std::uint16_t>(b_[p_] | (b_[p_ + 1] << 8));
        p_ += 2;
        return v;
    }
    std::uint32_t u32() {
        need(4);
        const std::uint32_t v = static_cast<std::uint32_t>(b_[p_]) | (static_cast<std::uint32_t>(b_[p_ + 1]) << 8) |
            (static_cast<std::uint32_t>(b_[p_ + 2]) << 16) | (static_cast<std::uint32_t>(b_[p_ + 3]) << 24);
        p_ += 4;
        return v;
    }
    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }
    std::vector<std::uint8_t> raw(std::size_t n) {
        need(n);
        std::vector<std::uint8_t> v(b_.begin() + static_cast<std::ptrdiff_t>(p_), b_.begin() + static_cast<std::ptrdiff_t>(p_ + n));
        p_ += n;
        return v;
    }
    // Slots +0x24 / +0x28 / +0x2C: u32 length, then the bytes (no terminator in the stream).
    std::string string() {
        const std::uint32_t n = u32();
        need(n);
        std::string s(b_.begin() + static_cast<std::ptrdiff_t>(p_), b_.begin() + static_cast<std::ptrdiff_t>(p_ + n));
        p_ += n;
        return s;
    }

private:
    void need(std::size_t n) const {
        if (n > remaining()) throw DmoError("truncated stream at logical offset " + std::to_string(p_));
    }
    const std::vector<std::uint8_t>& b_;
    std::size_t p_;
};

// TVXPlayerSettings::Serialize 0x00567600 (body from 0x00567630), one record per slot.
struct PlayerSettings {
    std::int32_t race = 0, allowed_races = 0, control = 0, real = 0, bonus_index = 0;
    std::string ai_script;
    std::int32_t difficulty = 0, start_x = 0, start_y = 0;
    std::string name;
    std::uint32_t color = 0;
    std::int32_t allied = 0, id = 0;
    std::array<std::int32_t, 16> relation{};
};

// Random-map settings, Persist 0x0057B1F0 (object constructed by 0x0057D9A0 at basic settings +0x2C).
struct RandomMapSettings {
    std::string template_path;
    std::uint32_t map_size = 0;
    std::array<std::uint8_t, 8> teams{};
    std::array<std::uint8_t, 8> races{};
    std::array<std::uint32_t, 12> flags{};  // object +0x38..+0x64
};

// Game data, Persist 0x005284F0 (thunk 0x00528750; object constructed by 0x00526C40 at basic settings +0xC9C).
struct GameData {
    std::uint32_t kind = 0;          // 0, 1 or 2; anything else is refused by the native reader
    std::array<std::string, 7> text; // six strings, then a seventh after the native name check
    std::uint32_t value_a = 0, value_b = 0, value_c = 0, value_d = 0;
};

// TVXGameSettings::Serialize 0x005BDD40 (vtable slot +4 of TVXGameSingleSettings 0x007C7BF4), which first runs
// TVXBasicGameSettings::Serialize 0x005BDB60.
struct GameSettings {
    std::uint16_t class_id = 0;
    std::string base_map;
    std::uint32_t animate_decors = 0, random = 0;
    RandomMapSettings random_map;
    std::array<PlayerSettings, 16> players;
    GameData game_data;
    std::array<std::uint32_t, 8> basic_flags{};  // basic settings +0xD90..+0xDAC (labels include fogofwar, loadgame)
    std::uint32_t basic_value_a = 0, basic_value_b = 0;  // +0xDB0, +0xDB4
    std::uint32_t colorize = 0;       // +0xDB8
    std::uint32_t normal_seed = 0;    // +0xDC0, label "normalseed"
    std::uint32_t sync_seed = 0;      // +0xDBC, label "syncseed"
    std::uint32_t play_demo = 0;      // +0xDC4, label "playdemo"
};

inline PlayerSettings read_player_settings(StreamReader& r) {
    PlayerSettings p;
    p.race = r.i32();
    p.allowed_races = r.i32();
    p.control = r.i32();
    p.real = r.i32();
    p.bonus_index = r.i32();
    p.ai_script = r.string();
    p.difficulty = r.i32();
    p.start_x = r.i32();
    p.start_y = r.i32();
    p.name = r.string();
    p.color = r.u32();
    p.allied = r.i32();
    p.id = r.i32();
    for (auto& v : p.relation) v = r.i32();
    return p;
}

inline RandomMapSettings read_random_map_settings(StreamReader& r) {
    RandomMapSettings s;
    s.template_path = r.string();
    s.map_size = r.u32();
    for (auto& v : s.teams) v = r.u8();
    for (auto& v : s.races) v = r.u8();
    for (auto& v : s.flags) v = r.u32();
    return s;
}

inline GameData read_game_data(StreamReader& r) {
    GameData g;
    const std::size_t at = r.pos();
    g.kind = r.u32();
    if (g.kind > 2) throw DmoError("game data kind " + std::to_string(g.kind) + " at logical offset " + std::to_string(at));
    for (auto& s : g.text) s = r.string();
    g.value_a = r.u32();
    g.value_b = r.u32();
    g.value_c = r.u32();
    g.value_d = r.u32();
    return g;
}

// CGameReplayData::SerializeSettingsRef 0x0058D340: the settings object through slot +0x54.
// The object class id is kept, not interpreted: every settings class (single, multi, adv) shares the Persist
// 0x005BDD40, and Game_LoadDemoReplay only requires a TVXGameSingleSettings afterwards.
inline GameSettings read_game_settings(StreamReader& r) {
    GameSettings s;
    const std::size_t at = r.pos();
    s.class_id = r.u16();
    if (s.class_id == 0xFFFF || s.class_id == 0xFFFE)
        throw DmoError("no settings object at logical offset " + std::to_string(at));
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

// One serialized command order (CCmdRegistry::SerializeRegisteredCommandOrder 0x004E7800: u16 type, then the
// payload reader at vtable slot +0x14 of the class the factory creates). Fields are kept by their native names.
struct OrderField {
    std::string name;
    std::string value;
};

struct Order {
    std::size_t entry = 0, pool = 0;
    std::size_t start = 0, end = 0;  // logical offsets of the type word and of the byte after the payload
    std::uint16_t type = 0;
    std::uint8_t player_id = 0;
    std::vector<OrderField> fields;
    bool synced = true;  // F13: the registry's synced flag of the type (order_type_synced); playback skips unsynced orders
};

// CCmdQueueSet::SerializeCommandSnapshot 0x004F6BD0 plus the two dwords of SerializeCommandState 0x0058D8C0.
struct Entry {
    std::array<std::uint32_t, 8> game_hashes{};  // GameHashes section 0x00528780 (set +0xC8..+0xE4)
    std::array<std::uint32_t, 16> pool_counts{};
    std::uint32_t tick_exec_time = 0;  // snapshot +0xF0
    std::uint32_t marker = 0;          // snapshot +0xF4, "droM" (0x4D6F7264) when written by the command pump
};

struct Replay {
    std::uint32_t magic = 0, build = 0, game_type = 0;
    GameSettings settings;
    std::size_t entry_count_offset = 0;
    std::uint32_t entry_count = 0;
    std::vector<Entry> entries;
    std::vector<Order> orders;
    std::uint32_t saved_time = 0;  // g_pGame+0x1258 when written
    std::size_t end_offset = 0;    // logical offset after saved_time
};

namespace detail {

inline void field(Order& o, const char* name, std::int64_t value) { o.fields.push_back({name, std::to_string(value)}); }

inline std::string hex_bytes(const std::vector<std::uint8_t>& bytes) {
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const auto b : bytes) {
        if (!out.empty()) out += ' ';
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    return out;
}

// CVXCmdBase::Serialize 0x004E78F0: the player id byte (slot +0x30).
inline void read_base(StreamReader& r, Order& o) { o.player_id = r.u8(); }

// CVXCmdUser::Serialize 0x004E79D0: base, a modifier byte (slot +0x38), then x (bit 0x04), y (bit 0x08) and the
// target handle (bit 0x10) as 16-bit values (slot +0x3C); x and y are sign-extended on load (0x004E7AE7).
inline void read_user(StreamReader& r, Order& o) {
    read_base(r, o);
    const std::uint8_t modifiers = r.u8();
    field(o, "modifiers", modifiers);
    if (modifiers & 0x04) field(o, "x", static_cast<std::int16_t>(r.u16()));
    if (modifiers & 0x08) field(o, "y", static_cast<std::int16_t>(r.u16()));
    if (modifiers & 0x10) field(o, "target_handle", r.u16());
}

// CVXCmdUI::Serialize 0x004E7B60: user fields, then the UI command index (slot +0x3C).
inline void read_ui(StreamReader& r, Order& o) {
    read_user(r, o);
    field(o, "ui_idx", r.u16());
}

// CVXCmdSelAdd 0x004E7080 / CVXCmdSelRemove 0x004E7180: base, a u16 count, then that many u16 handles (slot +0x18).
inline void read_handles(StreamReader& r, Order& o) {
    read_base(r, o);
    const std::uint16_t n = r.u16();
    std::string list;
    for (std::uint16_t i = 0; i < n; ++i) list += (i ? " " : "") + std::to_string(r.u16());
    field(o, "num_handles", n);
    o.fields.push_back({"handles", list});
}

}  // namespace detail

// Class name registered for each order type by CCmdRegistry 0x004EE870 (name pointers 0x0081B27C..0x0081B2EC);
// nullptr for an unregistered type (11, and everything above 22).
inline const char* order_class_name(std::uint16_t type) {
    static const char* const names[23] = {"CVXCmdUI", "CVXCmdDefault", "CVXCmdCancelCmd", "CVXCmdSetSpeed",
        "CVXCmdSelFilter", "CVXCmdSelAdd", "CVXCmdSelRemove", "CVXCmdSelShortcut", "CVXCmdSelSetNum",
        "CVXCmdUseItem", "CVXCmdGiveItem", nullptr, "CVXCmdToggleFog", "CVXCmdSummonObject", "CVXCmdMessage",
        "CVXCmdUIMultiple", "CVXChatMessage", "CVXDiplomacyChange", "CVXPlayerWantsToQuit", "CVXCmdPlayerSurrender",
        "CVXNetworkSplit", "CVXCmdSetFog", "CVXCmdSkillPlus"};
    return type < 23 ? names[type] : nullptr;
}

// F13: RegisterCommands 0x004EE870 stores a synced flag per registered type in the registry map (-0x20): 0 for
// CVXCmdSetSpeed (3), CVXCmdToggleFog (12) and CVXCmdSetFog (21), 1 for the other nineteen. IsCommandSynced
// 0x004E94B0 returns it; CVXCmdPump::Tick skips unsynced orders in playback ("Skipped command", 0x004F65F1) and
// live play executes them outside the command hash (claim:command-registry-marks-set-speed-and-fog-orders-unsynced).
// False for an unregistered type, which has no registry entry.
inline bool order_type_synced(std::uint16_t type) {
    return order_class_name(type) != nullptr && type != 3 && type != 12 && type != 21;
}

// The payload reader of each registered type: factory (registry map +0x34, filled by 0x004EE870) -> vtable ->
// slot +0x14. Field names are the stream labels where the reader passes one.
inline Order read_order(StreamReader& r) {
    using namespace detail;
    Order o;
    o.start = r.pos();
    o.type = r.u16();
    o.synced = order_type_synced(o.type);
    switch (o.type) {
    case 0: read_ui(r, o); break;                                       // 0x004E7B60
    case 1: read_user(r, o); break;                                     // 0x004E7C50 -> 0x004E79D0
    case 2:                                                             // 0x004E5680
        read_base(r, o);
        field(o, "target", r.u16());
        field(o, "cmd_id", r.u32());
        break;
    case 3: read_base(r, o); field(o, "speed", r.u32()); break;         // 0x004E5740
    case 4: case 12: case 19: read_base(r, o); break;                   // 0x004E78F0 (base only)
    case 5: case 6: read_handles(r, o); break;                          // 0x004E7080, 0x004E7180
    case 7:                                                             // 0x004E58B0
        read_base(r, o);
        field(o, "number", r.u8());
        field(o, "add_selection", r.u32());
        break;
    case 8: read_base(r, o); field(o, "number", r.u8()); break;         // 0x004E5A70
    case 9: read_user(r, o); field(o, "item_handle", r.u16()); break;   // 0x004E5B20
    case 10:                                                            // 0x004E5BD0
        read_base(r, o);
        field(o, "giver_handle", r.u16());
        field(o, "item_handle", r.u16());
        field(o, "taker_handle", r.u16());
        o.fields.push_back({"point", hex_bytes(r.raw(8))});             // slot +0x1C, eight bytes
        break;
    case 13:                                                            // 0x004E5D80
        read_base(r, o);
        field(o, "player", r.u32());
        o.fields.push_back({"point", hex_bytes(r.raw(8))});
        o.fields.push_back({"class", r.string()});
        break;
    case 14: read_base(r, o); o.fields.push_back({"message", r.string()}); break;  // 0x004E5E00
    case 15: read_ui(r, o); field(o, "count", r.u8()); break;           // 0x004E5E90
    case 16:                                                            // 0x004E5F00
        read_base(r, o);
        o.fields.push_back({"message", r.string()});
        o.fields.push_back({"location", hex_bytes(r.raw(8))});
        field(o, "playersmask", r.u16());
        break;
    case 17:                                                            // 0x004E6030
        read_base(r, o);
        field(o, "player1", r.u32());
        field(o, "player2", r.u32());
        field(o, "relations", r.u32());
        field(o, "allied", r.u32());
        break;
    case 18:                                                            // 0x004E6180
        read_base(r, o);
        field(o, "player", r.u32());
        field(o, "player_to_quit", r.u32());
        break;
    case 20: read_base(r, o); field(o, "forget_mask", r.u32()); break;  // 0x004E61F0
    case 21: read_base(r, o); field(o, "fog", r.u32()); break;          // 0x004E5D20
    case 22:                                                            // 0x004E62A0
        read_base(r, o);
        field(o, "hero_handle", r.u16());
        field(o, "value_10", r.u32());
        field(o, "value_14", r.u8());
        break;
    default:
        throw DmoError("unregistered order type " + std::to_string(o.type) + " at logical offset " + std::to_string(o.start));
    }
    o.end = r.pos();
    return o;
}

// CGameReplayData::SerializeCommandState 0x0058D8C0 after the count: per entry the snapshot of
// CCmdQueueSet::SerializeCommandSnapshot 0x004F6BD0 (GameHashes, then sixteen NetTick pools, each a u32 count and
// that many orders through the registry slot +0x2C), then tick_exec_time and the marker; then saved_time.
inline void read_command_state(StreamReader& r, Replay& rep) {
    for (std::uint32_t e = 0; e < rep.entry_count; ++e) {
        Entry entry;
        for (auto& h : entry.game_hashes) h = r.u32();
        for (std::size_t pool = 0; pool < entry.pool_counts.size(); ++pool) {
            entry.pool_counts[pool] = r.u32();
            for (std::uint32_t i = 0; i < entry.pool_counts[pool]; ++i) {
                Order o = read_order(r);
                o.entry = e;
                o.pool = pool;
                rep.orders.push_back(std::move(o));
            }
        }
        entry.tick_exec_time = r.u32();
        entry.marker = r.u32();
        rep.entries.push_back(entry);
    }
    rep.saved_time = r.u32();
    rep.end_offset = r.pos();
}

// Game_LoadDemoReplay 0x00596960: magic, build and game type (the native loader requires gsxv, the binary's
// network protocol version and type 4), then the settings, then the command state.
inline Replay read_replay(const std::vector<std::uint8_t>& logical) {
    StreamReader r(logical);
    Replay rep;
    rep.magic = r.u32();
    rep.build = r.u32();
    rep.game_type = r.u32();
    if (rep.magic != kMagic) throw DmoError("not a replay logical stream (magic gsxv)");
    if (rep.game_type != 4) throw DmoError("game type " + std::to_string(rep.game_type) + " is not a replay (4)");
    rep.settings = read_game_settings(r);
    rep.entry_count_offset = r.pos();
    rep.entry_count = r.u32();  // CGameReplayData::SerializeCommandState 0x0058D8C0: count through slot +0x44
    read_command_state(r, rep);
    return rep;
}

}  // namespace iberivm::dmo
