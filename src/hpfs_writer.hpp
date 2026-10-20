// W4 HPFS (.BFHP) volumes as ordered trees: read, edit, write and structural check. A line-by-line port of
// Iberivm tools/randommap/bfhp.py (read_tree, replace_files, write_volume, validate), which the game loads (load
// tests LT01-LT06); keep the two identical so the written volumes stay byte-equal.
//
// Layout: page 0 holds the 0x20-byte header (magic `HPFS`, page size 512, page count, page limit, free-page count,
// first free-list page, descriptor header size 8, root descriptor page). A file is a descriptor page (u32 size,
// u16 index level, u16 0, then page pointers from word 2); a page index is split into base-128 digits, the top one
// indexing the descriptor (126 slots), each lower one an index page of 128 pointers. A directory is a file of
// entries: u32 descriptor page, u16 flags (1 = directory), u16 name length, name bytes.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace mapx::hpfs {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::size_t kSector = 512;
inline constexpr std::size_t kPointers = kSector / 4;      // pointers per index page
inline constexpr std::size_t kTopSlots = (kSector - 8) / 4;  // pointer slots in a descriptor

// One directory entry; a directory keeps its children in order (empty directories included).
struct Node {
    std::string name;
    bool is_dir = false;
    Bytes data;
    std::vector<Node> children;

    Node* child(const std::string& n) {
        for (auto& c : children)
            if (c.name == n) return &c;
        return nullptr;
    }
};

namespace detail {

inline std::uint32_t le32(const Bytes& b, std::size_t at) {
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
        static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
inline std::uint16_t le16(const Bytes& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }
inline void put32(Bytes& b, std::uint32_t v) {
    for (int k = 0; k < 4; ++k) b.push_back(static_cast<std::uint8_t>(v >> (8 * k)));
}
inline void put16(Bytes& b, std::uint16_t v) { b.push_back(static_cast<std::uint8_t>(v)), b.push_back(static_cast<std::uint8_t>(v >> 8)); }

// Base-128 digits of a data-page index, most significant first (CBFHP::ResolveOffset).
inline std::vector<std::size_t> digits(std::size_t p, unsigned level) {
    std::vector<std::size_t> out(level + 1);
    for (unsigned i = 0; i <= level; ++i) {
        out[level - i] = p % kPointers;
        p /= kPointers;
    }
    if (p) throw std::runtime_error("page index exceeds the index level");
    return out;
}

class Pager {
public:
    explicit Pager(const Bytes& raw) : raw_(raw), total_((raw.size() + kSector - 1) / kSector) {}
    // A page as 512 bytes, zero-padded at the end of the file.
    Bytes sec(std::size_t i) const {
        if (!(0 < i && i < total_)) throw std::runtime_error("page " + std::to_string(i) + " outside the volume");
        Bytes out(raw_.begin() + static_cast<std::ptrdiff_t>(i * kSector),
                  raw_.begin() + static_cast<std::ptrdiff_t>(std::min(raw_.size(), (i + 1) * kSector)));
        out.resize(kSector, 0);
        return out;
    }
    std::size_t total() const { return total_; }

private:
    const Bytes& raw_;
    std::size_t total_;
};

struct FilePages {
    std::uint32_t size = 0;
    std::vector<std::uint32_t> data;
    std::vector<std::uint32_t> index;  // sorted, unique
};

inline FilePages pages_of(const Pager& sec, std::uint32_t desc) {
    const Bytes d = sec.sec(desc);
    FilePages out;
    out.size = le32(d, 0);
    const unsigned level = le16(d, 4);
    std::map<std::uint32_t, bool> index;
    for (std::size_t p = 0; p < (out.size + kSector - 1) / kSector; ++p) {
        const auto dg = digits(p, level);
        if (dg[0] >= kTopSlots)
            throw std::runtime_error("descriptor " + std::to_string(desc) + ": page " + std::to_string(p) + " exceeds the level capacity");
        std::uint32_t page = desc;
        std::size_t word = dg[0] + 2;
        for (std::size_t k = 1; k < dg.size(); ++k) {
            page = le32(sec.sec(page), word * 4);
            index[page] = true;
            word = dg[k];
        }
        out.data.push_back(le32(sec.sec(page), word * 4));
    }
    for (const auto& [p, _] : index) out.index.push_back(p);
    return out;
}

inline Bytes read_file(const Pager& sec, std::uint32_t desc) {
    const auto fp = pages_of(sec, desc);
    Bytes out;
    for (const auto p : fp.data) {
        const auto s = sec.sec(p);
        out.insert(out.end(), s.begin(), s.end());
    }
    out.resize(fp.size);
    return out;
}

struct Header {
    std::uint32_t count = 0, limit = 0, free = 0, head = 0, hsize = 0, root = 0;
};

inline Header header(const Bytes& raw) {
    if (raw.size() < 0x20) throw std::runtime_error("shorter than the HPFS header");
    if (!(raw[0] == 'H' && raw[1] == 'P' && raw[2] == 'F' && raw[3] == 'S') || le32(raw, 4) != kSector)
        throw std::runtime_error("not an HPFS volume");
    return {le32(raw, 8), le32(raw, 12), le32(raw, 16), le32(raw, 20), le32(raw, 24), le32(raw, 28)};
}

}  // namespace detail

// The whole volume as an ordered tree, empty directories included (bfhp.read_tree).
inline Node read_tree(const Bytes& raw) {
    const auto h = detail::header(raw);
    const detail::Pager sec(raw);
    auto walk = [&](auto&& self, std::uint32_t desc, int depth) -> Node {
        if (depth > 64) throw std::runtime_error("directory nesting too deep");
        Node out;
        out.is_dir = true;
        const auto listing = detail::read_file(sec, desc);
        std::size_t o = 0;
        while (o + 8 <= listing.size()) {
            const auto target = detail::le32(listing, o);
            const auto flags = detail::le16(listing, o + 4);
            const auto length = detail::le16(listing, o + 6);
            o += 8;
            if (target == 0 && length == 0) break;
            if (o + length > listing.size()) throw std::runtime_error("directory entry past the listing");
            Node child = (flags & 1) ? self(self, target, depth + 1) : Node{};
            if (!(flags & 1)) child.data = detail::read_file(sec, target);
            child.name.assign(listing.begin() + static_cast<std::ptrdiff_t>(o), listing.begin() + static_cast<std::ptrdiff_t>(o + length));
            o += length;
            out.children.push_back(std::move(child));
        }
        return out;
    };
    return walk(walk, h.root, 0);
}

// {relative/path: bytes} in tree order (bfhp.flatten / read_volume).
inline std::vector<std::pair<std::string, Bytes>> flatten(const Node& tree, const std::string& prefix = "") {
    std::vector<std::pair<std::string, Bytes>> out;
    for (const auto& c : tree.children) {
        if (c.is_dir) {
            auto sub = flatten(c, prefix + c.name + "/");
            out.insert(out.end(), std::make_move_iterator(sub.begin()), std::make_move_iterator(sub.end()));
        } else {
            out.emplace_back(prefix + c.name, c.data);
        }
    }
    return out;
}

// Replace or add one file (bfhp.replace_files for one path): existing names keep their place, new ones go to the end
// of their directory, missing directories are created at the end of their parent.
inline void put_file(Node& tree, const std::string& path, const Bytes& data) {
    Node* d = &tree;
    std::size_t start = 0;
    for (auto slash = path.find('/'); slash != std::string::npos; slash = path.find('/', start)) {
        const auto part = path.substr(start, slash - start);
        start = slash + 1;
        Node* next = d->child(part);
        if (!next) {
            d->children.push_back(Node{part, true, {}, {}});
            next = &d->children.back();
        }
        if (!next->is_dir) throw std::runtime_error(path + ": " + part + " is a file");
        d = next;
    }
    const auto name = path.substr(start);
    if (Node* f = d->child(name)) {
        f->is_dir = false;
        f->data = data;
        f->children.clear();
    } else {
        d->children.push_back(Node{name, false, data, {}});
    }
}

inline void remove_child(Node& dir, const std::string& name) {
    for (auto it = dir.children.begin(); it != dir.children.end(); ++it)
        if (it->name == name) {
            dir.children.erase(it);
            return;
        }
}

// A compact volume: header, then each directory or file as descriptor, index pages and data, depth first in tree
// order (root descriptor on page 1, no free pages) (bfhp.write_volume).
inline Bytes write_volume(const Node& tree) {
    std::vector<Bytes> pages(1);  // page 0: header, written last
    auto alloc = [&]() -> std::uint32_t {
        pages.emplace_back();
        return static_cast<std::uint32_t>(pages.size() - 1);
    };
    auto put_data = [&](const Bytes& data, std::uint32_t desc) {
        const std::size_t n = (data.size() + kSector - 1) / kSector;
        unsigned level = 0;
        for (std::size_t cap = kTopSlots; n > cap; cap *= kPointers) ++level;
        std::vector<std::uint32_t> slots(kTopSlots, 0);
        std::map<std::vector<std::size_t>, std::uint32_t> index;  // digit prefix -> index page
        std::map<std::uint32_t, std::vector<std::uint32_t>> words;
        for (std::size_t p = 0; p < n; ++p) {
            const auto dg = detail::digits(p, level);
            for (unsigned k = 1; k <= level; ++k) {
                const std::vector<std::size_t> prefix(dg.begin(), dg.begin() + k);
                if (index.count(prefix)) continue;
                const auto ip = alloc();
                index[prefix] = ip;
                words[ip] = std::vector<std::uint32_t>(kPointers, 0);
                if (k == 1) slots[dg[0]] = ip;
                else words[index[std::vector<std::size_t>(prefix.begin(), prefix.end() - 1)]][dg[k - 1]] = ip;
            }
            const auto dp = alloc();
            if (level == 0) slots[dg[0]] = dp;
            else words[index[std::vector<std::size_t>(dg.begin(), dg.end() - 1)]][dg.back()] = dp;
            Bytes chunk(data.begin() + static_cast<std::ptrdiff_t>(p * kSector),
                        data.begin() + static_cast<std::ptrdiff_t>(std::min(data.size(), (p + 1) * kSector)));
            chunk.resize(kSector, 0);
            pages[dp] = std::move(chunk);
        }
        Bytes d;
        detail::put32(d, static_cast<std::uint32_t>(data.size()));
        detail::put16(d, static_cast<std::uint16_t>(level));
        detail::put16(d, 0);
        for (const auto s : slots) detail::put32(d, s);
        pages[desc] = std::move(d);
        for (const auto& [ip, w] : words) {
            Bytes b;
            for (const auto v : w) detail::put32(b, v);
            pages[ip] = std::move(b);
        }
    };
    auto put_dir = [&](auto&& self, const Node& dir, std::uint32_t desc) -> void {
        Bytes entries;
        for (const auto& c : dir.children) {
            const auto child = alloc();
            if (c.is_dir) self(self, c, child);
            else put_data(c.data, child);
            detail::put32(entries, child);
            detail::put16(entries, c.is_dir ? 1 : 0);
            detail::put16(entries, static_cast<std::uint16_t>(c.name.size()));
            entries.insert(entries.end(), c.name.begin(), c.name.end());
        }
        put_data(entries, desc);
    };
    put_dir(put_dir, tree, alloc());
    Bytes h{'H', 'P', 'F', 'S'};
    for (const std::uint32_t v : {static_cast<std::uint32_t>(kSector), static_cast<std::uint32_t>(pages.size()), 0xFFFFFFFFu, 0u, 0u, 8u, 1u})
        detail::put32(h, v);
    h.resize(kSector, 0);
    pages[0] = std::move(h);
    Bytes out;
    out.reserve(pages.size() * kSector);
    for (const auto& p : pages) {
        if (p.size() != kSector) throw std::runtime_error("internal: page of " + std::to_string(p.size()) + " bytes");
        out.insert(out.end(), p.begin(), p.end());
    }
    return out;
}

// What the game's reader or allocator would trip on (bfhp.validate, problems only): header fields, descriptor levels
// and capacities, page bounds, a page used twice, duplicate names, a broken free-list chain.
inline std::vector<std::string> validate(const Bytes& raw) {
    std::vector<std::string> problems;
    detail::Header h;
    try {
        h = detail::header(raw);
    } catch (const std::exception& e) {
        return {e.what()};
    }
    const detail::Pager sec(raw);
    if (h.hsize != 8) problems.push_back("descriptor header size " + std::to_string(h.hsize) + " (8 expected)");
    if (h.count > sec.total()) problems.push_back("page count beyond the file");
    if (h.count > h.limit) problems.push_back("page count above the limit");
    std::map<std::uint32_t, std::string> owner;
    auto claim = [&](std::uint32_t p, const std::string& role) {
        if (!(0 < p && p < h.count)) problems.push_back(role + ": page " + std::to_string(p) + " outside the volume");
        else if (owner.count(p)) problems.push_back(role + ": page " + std::to_string(p) + " already used as " + owner[p]);
        else owner[p] = role;
    };
    auto walk_file = [&](std::uint32_t desc, const std::string& name) -> Bytes {
        claim(desc, "descriptor " + name);
        if (detail::le16(sec.sec(desc), 6)) problems.push_back(name + ": descriptor word +6 is not zero");
        const auto fp = detail::pages_of(sec, desc);
        for (const auto ip : fp.index) claim(ip, "index " + name);
        for (const auto dp : fp.data) claim(dp, "data " + name);
        return detail::read_file(sec, desc);
    };
    auto walk_dir = [&](auto&& self, std::uint32_t desc, const std::string& name, int depth) -> void {
        if (depth > 64) throw std::runtime_error("directory nesting too deep");
        const auto listing = walk_file(desc, name + "/");
        std::size_t o = 0;
        std::map<std::string, bool> seen;
        while (o + 8 <= listing.size()) {
            const auto target = detail::le32(listing, o);
            const auto flags = detail::le16(listing, o + 4);
            const auto length = detail::le16(listing, o + 6);
            o += 8;
            std::string entry(listing.begin() + static_cast<std::ptrdiff_t>(std::min(o, listing.size())),
                              listing.begin() + static_cast<std::ptrdiff_t>(std::min(o + length, listing.size())));
            o += length;
            std::string key = entry;
            for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (seen.count(key)) problems.push_back(name + "/" + entry + ": duplicate name (case-insensitive)");
            seen[key] = true;
            if (flags & 1) self(self, target, name + "/" + entry, depth + 1);
            else walk_file(target, name + "/" + entry);
        }
        if (o != listing.size()) problems.push_back(name + "/: trailing bytes in the directory");
    };
    try {
        walk_dir(walk_dir, h.root, "", 0);
        // the head page holds (free - 1) % 128 free pages after its next pointer; every later page is full
        std::uint32_t left = h.free, page = h.head;
        while (left > 0) {
            claim(page, "free-list page");
            const auto w = sec.sec(page);
            const std::uint32_t k = (left - 1) % kPointers + 1;
            for (std::uint32_t i = 1; i < k; ++i) claim(detail::le32(w, i * 4), "free page");
            left -= k;
            page = detail::le32(w, 0);
        }
    } catch (const std::exception& e) {
        problems.push_back(e.what());
    }
    return problems;
}

}  // namespace mapx::hpfs
