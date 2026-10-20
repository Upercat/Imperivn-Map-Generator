// Derived map.obj.xml attribute table (the shape of tools/randommap/class_attrs.json) from DATA/CLASSES.
//
// C++ port of tools/randommap/class_attrs_derive.py (night track B; findings and VAs in
// research/class-attrs-derivation.md). The shipped class_attrs.json stays the oracle and the Python module stays
// the verifier: tools/test_class_attrs_cpp.py compares this table with both, field by field.
//
// Per class the table lists the attributes map.obj.xml writes for a freshly created object, in write order, as
// (name, value) pairs; "*" marks a value the writer fills per object (player, position, flags, facing, area fields).
// `flagsbase` is not written natively: it is the object's state word (+0x2C) before the writer masks out 0x10000
// and the generator adds the owner and member bits.
//
// Text matching follows the Python module exactly: files are decoded as latin-1 (one byte = one code point), and
// the regular expressions `<class\s(.*?)>`, `<properties(.*?)/>`, `([\w.]+)\s*=\s*"([^"]*)"` and
// `\s*([+-]?\d+)` use Python's Unicode classes restricted to latin-1, as do `str.lower()` and `int()`.
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "game_fs.hpp"

namespace iberivm::classattrs {

// Numeric class attributes start as this sentinel and stay there when no class in the chain sets them
// (CVXScriptClass constructor, cerebrivm struct:class-template).
constexpr long long kUnset = -15000000;

using AttrList = std::vector<std::pair<std::string, std::string>>;

struct DerivedClass {
    std::string cpp_class;
    AttrList attrs;
};
using DerivedTable = std::map<std::string, DerivedClass>;  // keyed by the class id as spelled in its file

namespace detail {

// Python str.isspace() over latin-1 code points (what `\s` matches in a str pattern).
inline bool py_space(unsigned char c) {
    return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F) || c == 0x85 || c == 0xA0;
}
// Python `\w` over latin-1 code points: str.isalnum() or '_' (letters, digits, and the latin-1 numerics
// superscripts and fractions).
inline bool py_word(unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == 0xAA ||
           c == 0xB2 || c == 0xB3 || c == 0xB5 || c == 0xB9 || c == 0xBA || (c >= 0xBC && c <= 0xBE) ||
           (c >= 0xC0 && c != 0xD7 && c != 0xF7);
}
// Python str.lower() over latin-1 code points.
inline std::string py_lower(std::string s) {
    for (auto& ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 0xC0 && c <= 0xDE && c != 0xD7)) ch = static_cast<char>(c + 0x20);
    }
    return s;
}

// dict(re.findall(r'([\w.]+)\s*=\s*"([^"]*)"', body)): insertion order of the first occurrence, last value wins.
inline AttrList attrs(const std::string& body) {
    AttrList out;
    auto word = [](unsigned char c) { return py_word(c) || c == '.'; };
    std::size_t p = 0;
    while (p < body.size()) {
        if (!word(static_cast<unsigned char>(body[p]))) {
            ++p;
            continue;
        }
        auto e = p;
        while (e < body.size() && word(static_cast<unsigned char>(body[e]))) ++e;
        auto q = e;
        while (q < body.size() && py_space(static_cast<unsigned char>(body[q]))) ++q;
        if (q < body.size() && body[q] == '=') {
            ++q;
            while (q < body.size() && py_space(static_cast<unsigned char>(body[q]))) ++q;
            if (q < body.size() && body[q] == '"') {
                const auto close = body.find('"', q + 1);
                if (close != std::string::npos) {
                    const auto key = body.substr(p, e - p);
                    auto value = body.substr(q + 1, close - q - 1);
                    const auto it = std::find_if(out.begin(), out.end(), [&](const auto& kv) { return kv.first == key; });
                    if (it != out.end()) it->second = std::move(value);
                    else out.emplace_back(key, std::move(value));
                    p = close + 1;
                    continue;
                }
            }
        }
        ++p;  // findall retries one character later (inside the same word the tail fails the same way)
    }
    return out;
}

inline const std::string* get(const AttrList& a, const std::string& k) {
    for (const auto& kv : a)
        if (kv.first == k) return &kv.second;
    return nullptr;
}

// re.search(r"<class\s(.*?)>", text, re.S): body of the first class tag.
inline std::optional<std::string> class_tag_body(const std::string& t) {
    for (std::size_t p = 0;;) {
        const auto b = t.find("<class", p);
        if (b == std::string::npos || b + 6 >= t.size()) return std::nullopt;
        if (py_space(static_cast<unsigned char>(t[b + 6]))) {
            const auto e = t.find('>', b + 7);
            if (e == std::string::npos) return std::nullopt;
            return t.substr(b + 7, e - b - 7);
        }
        p = b + 1;
    }
}

// Bodies of re.finditer(r"<properties(.*?)/>", text, re.S).
inline std::vector<std::string> properties_bodies(const std::string& t) {
    std::vector<std::string> out;
    const std::string open = "<properties";
    for (std::size_t p = 0;;) {
        const auto b = t.find(open, p);
        if (b == std::string::npos) break;
        const auto e = t.find("/>", b + open.size());
        if (e == std::string::npos) break;
        out.push_back(t.substr(b + open.size(), e - b - open.size()));
        p = e + 2;
    }
    return out;
}

// Python int(re.match(r"\s*([+-]?\d+)", v).group(1)) as a normalized decimal string, or "0" when it does not match.
inline std::string int_text(const std::string& v) {
    std::size_t p = 0;
    while (p < v.size() && py_space(static_cast<unsigned char>(v[p]))) ++p;
    bool neg = false;
    if (p < v.size() && (v[p] == '+' || v[p] == '-')) neg = v[p++] == '-';
    const auto d0 = p;
    while (p < v.size() && v[p] >= '0' && v[p] <= '9') ++p;
    if (p == d0) return "0";
    auto digits = v.substr(d0, p - d0);
    digits.erase(0, std::min(digits.find_first_not_of('0'), digits.size() - 1));
    return (neg && digits != "0" ? "-" : "") + digits;
}

// Saturating value of a normalized decimal string; exact for every comparison the derivation makes
// (with kUnset, 0 and 1).
inline long long int_value(const std::string& text) {
    const bool neg = !text.empty() && text[0] == '-';
    long long v = 0;
    for (std::size_t i = neg ? 1 : 0; i < text.size(); ++i) {
        if (v > 100000000000000000LL) {
            v = 100000000000000000LL;
            break;
        }
        v = v * 10 + (text[i] - '0');
    }
    return neg ? -v : v;
}

}  // namespace detail

// One class file: id, parent and cpp_class of its first <class> tag, properties of every <properties .../> of the
// file merged in file order (first occurrence wins, as objects.class_props reads them).
struct ClassRecord {
    std::string id, parent, cpp_class;
    AttrList props;
    std::string altid; // own alias, ClassTemplate+20; not inherited through parent chain
    // Every itemtype attribute in document order: OnXmlStartElement 005A1D57..005A1E56 assigns the
    // first over the "**Invalid**" sentinel and appends ", " plus each later one (template +6A0).
    std::vector<std::string> itemtypes;
};

class ClassRegistry {
public:
    // {class id or altid (lower case): record} from DATA/CLASSES, files in upper-case name order.
    // PROTOTYPE POLICY (carried over from class_attrs_derive.py): a duplicate id keeps the first file in name
    // order; the native registry's duplicate rule is not established (no class of class_attrs.json has a
    // duplicate id).
    explicit ClassRegistry(const GameFS& fs) {
        auto files = fs.list_files("DATA/CLASSES");
        auto upper_name = [](const std::string& k) {
            auto n = k.substr(k.rfind('/') == std::string::npos ? 0 : k.rfind('/') + 1);
            for (auto& ch : n)
                if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
            return n;
        };
        std::stable_sort(files.begin(), files.end(), [&](const auto& a, const auto& b) { return upper_name(a) < upper_name(b); });
        for (const auto& f : files) {
            const auto text = fs.read_text(f);
            const auto body = detail::class_tag_body(text);
            if (!body) continue;
            const auto a = detail::attrs(*body);
            ClassRecord rec;
            if (const auto* v = detail::get(a, "id")) rec.id = *v;
            if (const auto* v = detail::get(a, "altid")) rec.altid = *v; //0059F660..0059F6BA
            if (const auto* v = detail::get(a, "parent")) rec.parent = *v;
            if (const auto* v = detail::get(a, "cpp_class")) rec.cpp_class = *v;
            for (const auto& pb : detail::properties_bodies(text))
                for (auto& [k, v] : detail::attrs(pb)) {
                    if (k == "itemtype") rec.itemtypes.push_back(v);
                    if (!detail::get(rec.props, k)) rec.props.emplace_back(k, v);
                }
            records_.push_back(std::move(rec));
            const std::size_t index = records_.size() - 1;
            for (const char* k : {"id", "altid"})
                if (const auto* key = detail::get(a, k); key && !key->empty()) by_key_.emplace(detail::py_lower(*key), index);
        }
    }

    const std::vector<ClassRecord>& records() const { return records_; }
    // Records reachable from at least one key (the Python registry's distinct values).
    std::vector<const ClassRecord*> reachable() const {
        std::vector<bool> seen(records_.size(), false);
        std::vector<const ClassRecord*> out;
        for (const auto& [k, i] : by_key_)
            if (!seen[i]) seen[i] = true, out.push_back(&records_[i]);
        return out;
    }
    const ClassRecord* find(const std::string& lower_key) const {
        const auto it = by_key_.find(lower_key);
        return it == by_key_.end() ? nullptr : &records_[it->second];
    }

private:
    std::vector<ClassRecord> records_;
    std::map<std::string, std::size_t> by_key_;
};

// One class with its parent chain resolved (child wins).
class ClassChain {
public:
    ClassChain(const std::string& name, const ClassRegistry& reg) {
        std::vector<std::string> seen;
        auto c = detail::py_lower(name);
        while (!c.empty() && std::find(seen.begin(), seen.end(), c) == seen.end()) {
            const auto* r = reg.find(c);
            if (!r) break;
            seen.push_back(c);
            chain_.push_back(r);
            c = detail::py_lower(r->parent);
        }
    }
    bool empty() const { return chain_.empty(); }
    const ClassRecord& self() const { return *chain_.front(); }

    std::optional<std::string> cpp_class() const {
        for (const auto* r : chain_)
            if (!r->cpp_class.empty()) return r->cpp_class;
        return std::nullopt;
    }
    const std::string* prop(const std::string& key) const {
        for (const auto* r : chain_)
            if (const auto* v = detail::get(r->props, key)) return v;
        return nullptr;
    }
    // The nearest class record that defines `key` (the record prop() reads), or null.
    const ClassRecord* prop_owner(const std::string& key) const {
        for (const auto* r : chain_)
            if (detail::get(r->props, key)) return r;
        return nullptr;
    }
    // Numeric class attribute as the native template holds it (`_atol`; unset = kUnset), as decimal text.
    std::string num_text(const std::string& key) const {
        const auto* v = prop(key);
        return v ? detail::int_text(*v) : std::to_string(kUnset);
    }
    long long num(const std::string& key) const { return detail::int_value(num_text(key)); }
    bool is_heir_of(const std::string& base) const {
        const auto b = detail::py_lower(base);
        for (const auto* r : chain_)
            if (detail::py_lower(r->id) == b) return true;
        return false;
    }

private:
    std::vector<const ClassRecord*> chain_;
};

inline bool is_unit_class(const std::optional<std::string>& cc) { return cc && (*cc == "CVXUnit" || *cc == "CVXFlyingUnit"); }

// State word +0x2C of a new object, before the owner and member bits.
inline std::uint32_t flags_base(const ClassChain& cd) {
    std::uint32_t flags = 0x80000000u;  // CVXMapObj constructor: MOV [ESI+0x2C], 0x80000000 at 0x0053D572
    if (is_unit_class(cd.cpp_class())) {
        flags |= 0x400000u;  // CVXUnit constructor ORs 0x400000 into +0x2C (0x005D3434..0x005D343E)
        // CVXUnit constructor: food +0x16C = class max_food (+0x2B4); food >= 1 (signed JGE against 1) sets
        // 0x20000000 through SetStateFlags (vtable +0x44), otherwise ClearStateFlags (+0x48), 0x005D327B..0x005D32DC.
        if (cd.num("max_food") >= 1) flags |= 0x20000000u;
    }
    return flags;
}

// CRandMapObjDef::CreateGroupMemberObject 0x005887F0 compares the created object's class name (template id, exact,
// case-sensitive std::string compare) with these five strings and, on a match, sets the experience to
// ExpTable__GetThreshold(10) through the virtual experience setter +0x118 (0x00588840..0x00588BE0).
// Strings: "Boar" 0x007C7148, "Bear" 0x007C7150, "Wolf" 0x007C7158, "LionM", "LionF" (inline literals).
inline constexpr const char* kRandomMapLevel10Classes[] = {"Boar", "Bear", "Wolf", "LionM", "LionF"};

// `Level` written by CVXUnit::SaveXml for an object created by the random-map generator: LevelFromExp([+0x180]) - 1
// (CALL 0x005D28C0, DEC EAX at 0x005DAF62). The CVXUnit constructor zeroes the experience (0x005D3298) and
// LevelFromExp(0) is 1, so Level 0; the random-map animals above get level 10 and write 9 (GetThreshold(n) reads
// thresholds[n - 1] at 0x005D2914; the threshold table is filled at run time, the oracle's Level 9 confirms it is
// strictly increasing there).
inline int level(const ClassChain& cd) {
    const bool level10 = std::any_of(std::begin(kRandomMapLevel10Classes), std::end(kRandomMapLevel10Classes),
                                     [&](const char* n) { return cd.self().id == n; });
    return (level10 ? 10 : 1) - 1;
}

// CVXUnit constructor 0x005D3170 heir tests (CVXScriptClass::IsHeirOfName 0x0059C020 walks the parent chain):
// "Boar" 0x007C7158, "Bear" 0x007C7150, "Wolf" 0x007C7148, "LionF" 0x007C7138, "LionM" 0x007C7140.
inline constexpr const char* kAttackAnimalBases[] = {"Boar", "Bear", "Wolf", "LionF", "LionM"};

// `UnitFlags` (+0x194) of a new unit, written by CVXUnit::SaveXml (MOV ECX,[EDI+0x194] at 0x005DB021).
inline std::uint32_t unit_flags(const ClassChain& cd) {
    std::uint32_t f = 0;                           // MOV [ESI+0x194], EDI with EDI = 0 (0x005D3314, XOR EDI,EDI at 0x005D318C)
    if (cd.is_heir_of("Sentry")) f |= 0x4040000u;  // CVXMapObj::IsSentry 0x005400C0; OR at 0x005D3323
    if (cd.num("feeds") != 0) f |= 0x20000u;       // class +0x670 (feeds) compared with 0, unset counts as set (0x005D3330..0x005D3345)
    f &= ~0x380000u;                               // AND EAX, 0xFFC7FFFF at 0x005D334A
    if (std::any_of(std::begin(kAttackAnimalBases), std::end(kAttackAnimalBases), [&](const char* b) { return cd.is_heir_of(b); }))
        f |= 0x200000u;  // 0x005D334F..0x005D33C2
    const auto ntd = cd.num("no_transparent_draw");  // class +0xB40
    if (ntd != kUnset && ntd != 0) f |= 0x2000000u;  // 0x005D33C8..0x005D33F4 (cleared otherwise)
    if (cd.cpp_class() == std::optional<std::string>("CVXFlyingUnit") && cd.num("initial_z") != kUnset)
        f |= 0x400000u;  // CVXFlyingUnit constructor: class +0xB44 set -> OR 0x400000 (0x0051AF5C..0x0051AF70)
    return f;
}

// Attribute layout per native C++ class (the SaveXml / WriteXmlAttributes override chain):
//   CVXMapObj::WriteXmlAttributes 0x0053F810: x, y, flags ([+0x2C] & ~0x10000, 0x0053F9F5..0x0053F9F8), dir.x, dir.y.
//   CVXScriptObj::WriteXmlAttributes 0x005AF760: player, healthperc, stamina only when
//     [+0xCC] != 0 (0x005AF917..0x005AF98E), inventorysize, slot%d per held item, then CVXMapObj.
//   CVXUnit::SaveXml 0x005DAE10: Level, UnitFlags, then CVXScriptObj.
// Which classes carry stamina, the TownHall `data` and the AdvArea area fields follow tools/randommap/HANDOFF.md B6
// (native saves). Known values (HANDOFF B6): buildings write stamina 20 whatever maxstamina says; TownHall writes
// data="-1" first; a new object has full health and an empty inventory.
struct Shape {
    const char* cpp_class;
    std::vector<const char*> prefix;
    enum class Stamina { none, building, unit } stamina;
    bool scriptobj;
};
inline const std::vector<Shape>& shapes() {
    using S = Shape::Stamina;
    static const std::vector<Shape> s{
        {"CVXDecor", {}, S::none, false},
        {"CVXScriptObj", {}, S::none, true},
        {"CVXItemHolder", {}, S::none, true},
        {"CVXBuilding", {}, S::building, true},
        {"CVXBarrack", {}, S::building, true},
        {"CVXGate", {}, S::building, true},
        {"CVXOutpost", {}, S::building, true},
        {"CVXTavern", {}, S::building, true},
        {"CVXTownHall", {"data"}, S::building, true},
        {"CVXUnit", {"Level", "UnitFlags"}, S::unit, true},
        {"CVXFlyingUnit", {"Level", "UnitFlags"}, S::unit, true},
        {"CVXAdvArea", {"nextmap", "targetarea", "type", "top", "bottom", "right", "left"}, S::none, true},
    };
    return s;
}
constexpr int kBuildingStamina = 20;

inline std::optional<DerivedClass> derive_class(const std::string& name, const ClassRegistry& reg) {
    const ClassChain cd(name, reg);
    if (cd.empty()) return std::nullopt;
    const auto cc = cd.cpp_class();
    if (!cc) return std::nullopt;
    const auto it = std::find_if(shapes().begin(), shapes().end(), [&](const Shape& s) { return *cc == s.cpp_class; });
    if (it == shapes().end()) return std::nullopt;
    DerivedClass out{*cc, {}};
    for (const std::string k : it->prefix) {
        if (k == "Level") out.attrs.emplace_back(k, std::to_string(level(cd)));
        else if (k == "UnitFlags") out.attrs.emplace_back(k, std::to_string(unit_flags(cd)));
        else out.attrs.emplace_back(k, k == "data" ? "-1" : "*");
    }
    if (it->scriptobj) {
        out.attrs.emplace_back("player", "*");
        out.attrs.emplace_back("healthperc", "100");
        if (it->stamina == Shape::Stamina::building) out.attrs.emplace_back("stamina", std::to_string(kBuildingStamina));
        else if (it->stamina == Shape::Stamina::unit)
            out.attrs.emplace_back("stamina", cd.num_text("maxstamina"));  // CVXScriptObj +0xC4 = template maxStamina (+0x298), struct:cvx-script-obj
        out.attrs.emplace_back("inventorysize", "0");
    }
    char hex[16];
    std::snprintf(hex, sizeof hex, "0x%08X", static_cast<unsigned>(flags_base(cd)));
    out.attrs.emplace_back("flags", "*");
    out.attrs.emplace_back("dir.x", "*");
    out.attrs.emplace_back("dir.y", "*");
    out.attrs.emplace_back("flagsbase", hex);
    return out;
}

// The derived table for every class of DATA/CLASSES whose C++ class has a known map.obj.xml shape.
inline DerivedTable derive(const GameFS& fs) {
    const ClassRegistry reg(fs);
    DerivedTable out;
    for (const auto* r : reg.reachable())
        if (auto d = derive_class(r->id, reg)) out[r->id] = std::move(*d);
    return out;
}

// (class -> attribute list) view used by the generator's map.obj.xml writer (rm::ClassAttrs has this type).
inline std::map<std::string, AttrList> attr_lists(const DerivedTable& t) {
    std::map<std::string, AttrList> out;
    for (const auto& [k, v] : t) out.emplace(k, v.attrs);
    return out;
}

}  // namespace iberivm::classattrs
