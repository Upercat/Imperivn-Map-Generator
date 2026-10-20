// Minimal JSON reader for tool-written case files (objects, arrays, strings, integers, numbers,
// true/false/null). Numbers keep their source text so integers stay exact.
#pragma once
#include <cctype>
#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace iberivm::json {

struct Value {
    enum class Kind { null, boolean, number, string, array, object } kind = Kind::null;
    bool b = false;
    std::string text;  // number source text or string value
    std::vector<Value> items;
    std::vector<std::pair<std::string, Value>> members;

    bool is_null() const { return kind == Kind::null; }
    const Value* find(const std::string& k) const {
        for (const auto& [key, v] : members)
            if (key == k) return &v;
        return nullptr;
    }
    const Value& at(const std::string& k) const {
        const auto* v = find(k);
        if (!v) throw std::runtime_error("json: missing key " + k);
        return *v;
    }
    const Value& operator[](std::size_t i) const { return items.at(i); }
    std::size_t size() const { return kind == Kind::array ? items.size() : members.size(); }
    std::int64_t as_int() const {
        if (kind == Kind::boolean) return b ? 1 : 0;
        if (kind != Kind::number) throw std::runtime_error("json: not a number");
        return std::stoll(text);
    }
    double as_double() const {
        if (kind != Kind::number) throw std::runtime_error("json: not a number");
        return std::stod(text);
    }
    const std::string& as_string() const {
        if (kind != Kind::string) throw std::runtime_error("json: not a string");
        return text;
    }
    bool truthy() const {
        switch (kind) {
            case Kind::null: return false;
            case Kind::boolean: return b;
            case Kind::number: return std::stod(text) != 0.0;
            case Kind::string: return !text.empty();
            case Kind::array: return !items.empty();
            case Kind::object: return !members.empty();
        }
        return false;
    }
};

class Reader {
public:
    explicit Reader(const std::string& s) : s_(s) {}
    Value document() {
        auto v = value();
        ws();
        if (p_ != s_.size()) throw std::runtime_error("json: trailing data");
        return v;
    }

private:
    const std::string& s_;
    std::size_t p_ = 0;
    void ws() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_;
    }
    char peek() {
        ws();
        if (p_ >= s_.size()) throw std::runtime_error("json: truncated");
        return s_[p_];
    }
    void expect(char c) {
        if (peek() != c) throw std::runtime_error(std::string("json: expected ") + c);
        ++p_;
    }
    std::string str() {
        expect('"');
        std::string out;
        while (p_ < s_.size() && s_[p_] != '"') {
            char c = s_[p_++];
            if (c == '\\') {
                const char e = s_.at(p_++);
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': {
                        const unsigned code = static_cast<unsigned>(std::stoul(s_.substr(p_, 4), nullptr, 16));
                        p_ += 4;
                        if (code < 0x80) out += static_cast<char>(code);
                        else if (code < 0x800) {
                            out += static_cast<char>(0xC0 | (code >> 6));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        } else {
                            out += static_cast<char>(0xE0 | (code >> 12));
                            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                            out += static_cast<char>(0x80 | (code & 0x3F));
                        }
                        break;
                    }
                    default: out += e;
                }
            } else
                out += c;
        }
        ++p_;
        return out;
    }
    Value value() {
        Value v;
        const char c = peek();
        if (c == '{') {
            v.kind = Value::Kind::object;
            ++p_;
            if (peek() == '}') {
                ++p_;
                return v;
            }
            for (;;) {
                auto k = str();
                expect(':');
                v.members.emplace_back(std::move(k), value());
                if (peek() == ',') {
                    ++p_;
                    continue;
                }
                expect('}');
                return v;
            }
        }
        if (c == '[') {
            v.kind = Value::Kind::array;
            ++p_;
            if (peek() == ']') {
                ++p_;
                return v;
            }
            for (;;) {
                v.items.push_back(value());
                if (peek() == ',') {
                    ++p_;
                    continue;
                }
                expect(']');
                return v;
            }
        }
        if (c == '"') {
            v.kind = Value::Kind::string;
            v.text = str();
            return v;
        }
        if (s_.compare(p_, 4, "true") == 0) {
            v.kind = Value::Kind::boolean;
            v.b = true;
            p_ += 4;
            return v;
        }
        if (s_.compare(p_, 5, "false") == 0) {
            v.kind = Value::Kind::boolean;
            p_ += 5;
            return v;
        }
        if (s_.compare(p_, 4, "null") == 0) {
            p_ += 4;
            return v;
        }
        v.kind = Value::Kind::number;
        const auto b = p_;
        while (p_ < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '-' || s_[p_] == '+' ||
                                  s_[p_] == '.' || s_[p_] == 'e' || s_[p_] == 'E'))
            ++p_;
        if (b == p_) throw std::runtime_error("json: unexpected character");
        v.text = s_.substr(b, p_ - b);
        return v;
    }
};

inline Value parse(const std::string& text) { return Reader(text).document(); }

}  // namespace iberivm::json
