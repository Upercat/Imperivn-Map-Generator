// Minimal XML element tree for the game's data files (random-map templates, PlaceTrees.xml,
// PlaceObjects.xml): elements, attributes (entity-decoded, in file order) and children; text,
// comments, processing instructions and DOCTYPE are skipped. Also a scanner that mirrors the
// regular expressions the Python reference uses on map.obj.xml, class and entity files.
#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace iberivm::xml {

struct Node {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<Node> children;

    const std::string* find_attr(const std::string& name) const {
        for (const auto& [k, v] : attrs)
            if (k == name) return &v;
        return nullptr;
    }
    std::string get(const std::string& name, const std::string& fallback = "") const {
        const auto* v = find_attr(name);
        return v ? *v : fallback;
    }
    bool has(const std::string& name) const { return find_attr(name) != nullptr; }
    const Node* child(const std::string& name) const {
        for (const auto& c : children)
            if (c.tag == name) return &c;
        return nullptr;
    }
    std::vector<const Node*> all(const std::string& name) const {
        std::vector<const Node*> out;
        for (const auto& c : children)
            if (c.tag == name) out.push_back(&c);
        return out;
    }
    // Every descendant (document order, like ElementTree.iter) with this tag, self included.
    void iter(const std::string& name, std::vector<const Node*>& out) const {
        if (tag == name) out.push_back(this);
        for (const auto& c : children) c.iter(name, out);
    }
};

inline std::string decode_entities(const std::string& s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            out += s[i];
            continue;
        }
        const auto semi = s.find(';', i);
        if (semi == std::string::npos) throw std::runtime_error("xml: bad entity");
        const auto name = s.substr(i + 1, semi - i - 1);
        if (name == "amp") out += '&';
        else if (name == "lt") out += '<';
        else if (name == "gt") out += '>';
        else if (name == "quot") out += '"';
        else if (name == "apos") out += '\'';
        else if (!name.empty() && name[0] == '#') {
            const unsigned long code = name.size() > 1 && (name[1] == 'x' || name[1] == 'X')
                                           ? std::stoul(name.substr(2), nullptr, 16)
                                           : std::stoul(name.substr(1));
            if (code > 0xFF) throw std::runtime_error("xml: character reference outside latin-1");
            out += static_cast<char>(code);
        } else
            throw std::runtime_error("xml: unknown entity &" + name + ";");
        i = semi;
    }
    return out;
}

class Parser {
public:
    explicit Parser(const std::string& text) : t_(text) {}
    Node parse_document() {
        for (;;) {
            skip_misc();
            if (p_ >= t_.size()) throw std::runtime_error("xml: no root element");
            if (t_[p_] == '<') return parse_element();
            ++p_;
        }
    }

private:
    const std::string& t_;
    std::size_t p_ = 0;

    static bool space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    void skip_space() {
        while (p_ < t_.size() && space(t_[p_])) ++p_;
    }
    bool starts(const char* s) const { return t_.compare(p_, std::char_traits<char>::length(s), s) == 0; }
    void skip_to(const char* s) {
        const auto q = t_.find(s, p_);
        if (q == std::string::npos) throw std::runtime_error(std::string("xml: missing ") + s);
        p_ = q + std::char_traits<char>::length(s);
    }
    // Skips text, comments, PIs and DOCTYPE until the next element tag or closing tag.
    void skip_misc() {
        for (;;) {
            while (p_ < t_.size() && t_[p_] != '<') ++p_;
            if (p_ >= t_.size()) return;
            if (starts("<!--")) skip_to("-->");
            else if (starts("<?")) skip_to("?>");
            else if (starts("<![CDATA[")) skip_to("]]>");
            else if (starts("<!")) skip_to(">");
            else return;
        }
    }
    std::string name() {
        const auto b = p_;
        while (p_ < t_.size() && !space(t_[p_]) && t_[p_] != '>' && t_[p_] != '/' && t_[p_] != '=') ++p_;
        return t_.substr(b, p_ - b);
    }
    Node parse_element() {
        Node n;
        ++p_;  // '<'
        n.tag = name();
        for (;;) {
            skip_space();
            if (p_ >= t_.size()) throw std::runtime_error("xml: truncated tag " + n.tag);
            if (t_[p_] == '/') {
                p_ += 2;  // "/>"
                return n;
            }
            if (t_[p_] == '>') {
                ++p_;
                break;
            }
            auto key = name();
            skip_space();
            if (p_ >= t_.size() || t_[p_] != '=') throw std::runtime_error("xml: attribute without value in " + n.tag);
            ++p_;
            skip_space();
            const char q = t_[p_++];
            const auto e = t_.find(q, p_);
            if (e == std::string::npos) throw std::runtime_error("xml: unterminated attribute");
            n.attrs.emplace_back(std::move(key), decode_entities(t_.substr(p_, e - p_)));
            p_ = e + 1;
        }
        for (;;) {
            skip_misc();
            if (p_ >= t_.size()) throw std::runtime_error("xml: unclosed element " + n.tag);
            if (starts("</")) {
                skip_to(">");
                return n;
            }
            n.children.push_back(parse_element());
        }
    }
};

inline Node parse(const std::string& text) { return Parser(text).parse_document(); }

// Python reference regex `([\w.]+)="([^"]*)"` over a tag body (values not entity-decoded).
inline std::vector<std::pair<std::string, std::string>> regex_attrs(const std::string& body) {
    std::vector<std::pair<std::string, std::string>> out;
    auto word = [](unsigned char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
               (c >= 0xC0 && c != 0xD7 && c != 0xF7) || c == 0xAA || c == 0xB5 || c == 0xBA;
    };
    std::size_t p = 0;
    while (p < body.size()) {
        if (!word(static_cast<unsigned char>(body[p]))) {
            ++p;
            continue;
        }
        auto e = p;
        while (e < body.size() && word(static_cast<unsigned char>(body[e]))) ++e;
        if (e + 1 < body.size() && body[e] == '=' && body[e + 1] == '"') {
            const auto close = body.find('"', e + 2);
            if (close != std::string::npos) {
                out.emplace_back(body.substr(p, e - p), body.substr(e + 2, close - e - 2));
                p = close + 1;
                continue;
            }
        }
        ++p;
    }
    return out;
}

inline const std::string* regex_get(const std::vector<std::pair<std::string, std::string>>& a, const std::string& k) {
    // dict(findall(...)): the last occurrence of a key wins
    const std::string* v = nullptr;
    for (const auto& [key, val] : a)
        if (key == k) v = &val;
    return v;
}

// Bodies of every `<tag(.*?)/>` (self-closing, non-greedy, across lines) from `from` to `to`.
inline std::vector<std::string> self_closing_bodies(const std::string& text, const std::string& tag, std::size_t from = 0,
                                                    std::size_t to = std::string::npos) {
    std::vector<std::string> out;
    if (to > text.size()) to = text.size();
    const std::string open = "<" + tag;
    std::size_t p = from;
    for (;;) {
        const auto b = text.find(open, p);
        if (b == std::string::npos || b >= to) break;
        const auto e = text.find("/>", b + open.size());
        if (e == std::string::npos || e + 2 > to) break;
        out.push_back(text.substr(b + open.size(), e - b - open.size()));
        p = e + 2;
    }
    return out;
}

}  // namespace iberivm::xml
