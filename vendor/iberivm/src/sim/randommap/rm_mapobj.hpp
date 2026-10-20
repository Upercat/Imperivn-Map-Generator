// map.obj.xml writer model (reference: objects.object_xml, objects.settlement_attrs, objects.write_map_obj;
// cerebrivm format:map-obj-xml, writer 0x0054C1A0, spatial walk 0x0054BF60, visitor 0x0054BED0). The
// per-class attribute table is tools/randommap/class_attrs.json (read-only input, collected from native saves).
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>
#include "json_lite.hpp"
#include "randommap_props.hpp"
#include "rm_objects.hpp"

namespace iberivm::rm {

using ClassAttrs = std::map<std::string, std::vector<std::pair<std::string, std::string>>>;

inline ClassAttrs parse_class_attrs(const std::string& json_text) {
    const auto doc = json::parse(json_text);
    ClassAttrs out;
    for (const auto& [cls, v] : doc.at("classes").members) {
        auto& list = out[cls];
        for (const auto& kv : v.at("attrs").items) list.emplace_back(kv[0].as_string(), kv[1].as_string());
    }
    return out;
}

constexpr int kIndependentVillagePop = 50;  // CONST.INI [GamePlay] IndependentVillagePop

class MapObjWriter {
public:
    MapObjWriter(const ClassAttrs& ca, rmdata::ClassProps& props, const Library& lib) : ca_(ca), props_(props), lib_(lib) {}

    std::string write(const std::vector<MapObject>& objects, const std::vector<Settlement>& settlements) {
        auto key = [&](std::size_t i) {
            const auto& o = objects[i];
            return std::make_tuple(o.y >> 8, o.x >> 8, i);
        };
        std::string out = "\t<mapobject>\r\n";
        long long num = 0;
        std::vector<std::size_t> loose;
        for (std::size_t i = 0; i < objects.size(); ++i)
            if (objects[i].kind != MapObject::Kind::member) loose.push_back(i);
        std::sort(loose.begin(), loose.end(), [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
        for (auto i : loose) out += object_xml(objects[i], num++);
        for (std::size_t sid = 0; sid < settlements.size(); ++sid) {
            const auto a = settlement_attrs(settlements[sid]);
            std::string body;
            for (const auto& [k, v] : a) body += "\t\t\t" + k + "=\"" + v + "\"\r\n";
            body = body.substr(0, body.size() - 2);
            out += "\t\t<settlement\r\n\t\t\tid=\"" + std::to_string(sid) + "\"\r\n" + body + ">\r\n";
            std::vector<std::size_t> members;
            for (std::size_t i = 0; i < objects.size(); ++i)
                if (objects[i].kind == MapObject::Kind::member && objects[i].settlement == static_cast<int>(sid)) members.push_back(i);
            std::sort(members.begin(), members.end(), [&](std::size_t a, std::size_t b) { return key(a) < key(b); });
            for (auto i : members) out += object_xml(objects[i], num++);
            out += "\t\t</settlement>\r\n";
        }
        out += "\t</mapobject>\r\n";
        return out;
    }

private:
    const ClassAttrs& ca_;
    rmdata::ClassProps& props_;
    const Library& lib_;

    std::string object_xml(const MapObject& o, long long num) {
        std::vector<std::pair<std::string, std::string>> ca;
        if (const auto it = ca_.find(o.cls); it != ca_.end()) ca = it->second;
        else if (props_.heir(o.cls, "Teleport"))
            // CVXTeleport writes destination_set (partner settlement id) before the building attributes (E03/E06 save)
            ca = {{"destination_set", "*"}, {"player", "*"}, {"healthperc", "100"}, {"stamina", "20"}, {"inventorysize", "0"},
                  {"flags", "*"},           {"dir.x", "*"},  {"dir.y", "*"},        {"flagsbase", "0x80000000"}};
        else
            throw std::runtime_error("class without map.obj.xml attributes: " + o.cls);
        std::vector<std::string> lines{"\t<scriptobj\r\n\t\tclass=\"" + o.cls + "\"\r\n\t\tnum=\"" + std::to_string(num) + "\""};
        std::optional<unsigned long long> flags;
        bool has_player = false;
        for (const auto& [k, v0] : ca) {
            std::string v = v0;
            if (k == "flagsbase") {
                flags = std::stoull(v, nullptr, 16);
                continue;
            }
            if (k == "player") v = std::to_string(o.owner + 1), has_player = true;
            else if (k == "x") v = std::to_string(o.x);
            else if (k == "y") v = std::to_string(o.y);
            else if (k == "destination_set") {
                if (o.partner < 0) throw std::runtime_error("teleport without partner");
                v = std::to_string(o.partner);
            } else if (k == "flags" || k == "dir.x" || k == "dir.y")
                continue;
            lines.push_back("\t" + k + "=\"" + v + "\"");
        }
        // x, y, flags, dir always close the element (CVXMapObj attributes)
        lines.erase(std::remove_if(lines.begin(), lines.end(), [](const std::string& ln) { return ln.rfind("\tx=", 0) == 0 || ln.rfind("\ty=", 0) == 0; }),
                    lines.end());
        if (!flags) throw std::runtime_error("class attributes without flagsbase: " + o.cls);
        const unsigned long long own = has_player ? (1ull << o.owner) : 0;
        const unsigned long long member = o.kind == MapObject::Kind::member ? 0x800000ull : 0;
        int dx = 0, dy = 1;
        if (o.has_facing) {
            dx = o.f2 - 15, dy = o.f1 - 15;
            if (dx == 0 && dy == 0) dy = 1;
        }
        char hex[32];
        std::snprintf(hex, sizeof hex, "0x%08llX", static_cast<unsigned long long>(*flags | member | own));
        lines.push_back("\tx=\"" + std::to_string(o.x) + "\"\r\n\ty=\"" + std::to_string(o.y) + "\"\r\n\tflags=\"" + hex + "\"\r\n\tdir.x=\"" +
                        std::to_string(dx) + "\"\r\n\tdir.y=\"" + std::to_string(dy) + "\"/>");
        std::string out;
        for (std::size_t i = 0; i < lines.size(); ++i) out += (i ? "\r\n" : "") + lines[i];
        return out + "\r\n";
    }

    std::vector<std::pair<std::string, std::string>> settlement_attrs(const Settlement& s) {
        // dict semantics: first occurrence keeps its position, last value wins
        std::vector<std::pair<std::string, std::string>> a;
        auto setv = [&](const std::string& k, const std::string& v) {
            for (auto& kv : a)
                if (kv.first == k) {
                    kv.second = v;
                    return;
                }
            a.emplace_back(k, v);
        };
        auto getv = [&](const std::string& k) -> std::string {
            for (const auto& kv : a)
                if (kv.first == k) return kv.second;
            throw std::runtime_error("settlement attribute missing: " + k);
        };
        auto getd = [&](const std::string& k, const std::string& d) {
            for (const auto& kv : a)
                if (kv.first == k) return kv.second;
            return d;
        };
        std::string cls;
        if (s.record) {
            for (const auto& [k, v] : *s.attrs) setv(k, v);
            cls = getv("classoffirstbuilding");
            // CVXSettlement::ConstructFromTemplate 0x005C3E60: max gold/food passed as -1, so the class defaults apply
            const auto mg = props_.prop(cls, "settlement_maxgold"), mf = props_.prop(cls, "settlement_maxfood");
            const auto maxgold = mg ? *mg : getv("maxgold"), maxfood = mf ? *mf : getv("maxfood");
            setv("maxgold", maxgold), setv("maxfood", maxfood);
            if (s.adjust && props_.heir(cls, "BaseTownhall")) {
                setv("maxpopulation", std::to_string(rmdata::py_int(getv("maxpopulation")) / 2));  // 0x0057BE80
                setv("population", "5");
                setv("gold", "0"), setv("food", "0");
            } else if (s.adjust && props_.heir(cls, "BaseVillage")) {
                const auto mp = rmdata::py_int(getv("maxpopulation"));
                const auto pop = static_cast<long long>(static_cast<double>(mp * kIndependentVillagePop) / 100.0);
                setv("population", std::to_string(std::max(pop, 10ll)));  // 0x0057BE80
            }
        } else {
            cls = s.cls;
            auto p = [&](const char* k, const char* d) {
                const auto v = props_.prop(cls, k);
                return v ? *v : std::string(d);
            };
            setv("maxpopulation", p("max_population", "0"));
            setv("extrasentries", "0");
            setv("maxgold", p("settlement_maxgold", "0"));
            setv("maxfood", p("settlement_maxfood", "0"));
            setv("population", p("population", "0"));
            setv("gold", p("settlement_gold", "0"));
            setv("food", p("settlement_food", "0"));
        }
        if (s.pop_bonus)  // 0x00589C20 house_pop_bonus of the last building
            setv("maxpopulation", std::to_string(rmdata::py_int(getv("maxpopulation")) + s.pop_bonus));
        return {{"player", std::to_string(s.owner + 1)}, {"classoffirstbuilding", cls},       {"maxpopulation", getv("maxpopulation")},
                {"extrasentries", getd("extrasentries", "0")}, {"maxgold", getv("maxgold")}, {"maxfood", getv("maxfood")},
                {"population", getv("population")},        {"gold", getv("gold")},             {"food", getv("food")},
                {"icon", ""},                              {"name", ""}};
    }
};

}  // namespace iberivm::rm
