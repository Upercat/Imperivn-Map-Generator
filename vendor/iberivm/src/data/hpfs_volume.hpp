// HPFS (.BFHP) volume reader (data module), the C++ twin of the reader half of tools/randommap/bfhp.py
// (read_tree / read_volume); keep the two identical. Layout read in /gbr.exe.aftersaul (CBFHP
// 0x00655DE0..0x006562E0, CBFHP::ResolveOffset 0x00657070, CHPFS 0x006400F0..0x00640D30):
//   - page 0 holds the 0x20-byte header: magic "HPFS", page size 512, page count, page limit, free-page
//     count, first free-list page, descriptor header size 8, root directory descriptor page;
//   - a file is a descriptor page: u32 size, u16 index level, u16 0, then page pointers from word 2. A data
//     page index splits into base-128 digits; the top digit indexes the descriptor (at most 126 slots), each
//     lower digit an index page of 128 pointers;
//   - a directory is a file of entries: u32 descriptor page, u16 flags (1 = directory), u16 name length, name
//     bytes; the file size ends the list, and an all-zero entry head stops it as in the Python reader.
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace iberivm::hpfs {

class HpfsError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr std::size_t kSector = 512;
inline constexpr std::size_t kPointers = kSector / 4;      // pointers per index page
inline constexpr std::size_t kTopSlots = (kSector - 8) / 4;  // pointer slots in a descriptor

namespace detail {

inline std::uint32_t le32(const std::uint8_t* p) {
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}
inline std::uint16_t le16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

class Pager {
public:
    explicit Pager(const std::vector<std::uint8_t>& raw) : raw_(raw), total_((raw.size() + kSector - 1) / kSector) {}
    // One page, zero-padded past the end of the volume (bfhp._pager).
    std::vector<std::uint8_t> page(std::uint32_t i) const {
        if (i == 0 || i >= total_) throw HpfsError("page " + std::to_string(i) + " outside the volume");
        std::vector<std::uint8_t> out(kSector, 0);
        const std::size_t start = static_cast<std::size_t>(i) * kSector;
        const std::size_t n = std::min(kSector, raw_.size() - start);
        std::memcpy(out.data(), raw_.data() + start, n);
        return out;
    }
    std::uint32_t word(std::uint32_t page_index, std::size_t word_index) const {
        return le32(page(page_index).data() + word_index * 4);
    }

private:
    const std::vector<std::uint8_t>& raw_;
    std::size_t total_;
};

// Base-128 digits of a data-page index, most significant first (CBFHP::ResolveOffset).
inline std::vector<std::size_t> digits(std::size_t p, unsigned level) {
    std::vector<std::size_t> out;
    for (unsigned i = 0; i <= level; ++i) {
        out.push_back(p % kPointers);
        p /= kPointers;
    }
    if (p) throw HpfsError("page index exceeds the index level");
    return {out.rbegin(), out.rend()};
}

inline std::vector<std::uint8_t> read_file(const Pager& pager, std::uint32_t desc) {
    const auto head = pager.page(desc);
    const std::uint32_t size = le32(head.data());
    const unsigned level = le16(head.data() + 4);
    std::vector<std::uint8_t> out;
    out.reserve(size);
    const std::size_t pages = (static_cast<std::size_t>(size) + kSector - 1) / kSector;
    for (std::size_t p = 0; p < pages; ++p) {
        const auto d = digits(p, level);
        if (d[0] >= kTopSlots)
            throw HpfsError("descriptor " + std::to_string(desc) + ": page exceeds the level capacity");
        std::uint32_t page = desc;
        std::size_t word = d[0] + 2;
        for (std::size_t k = 1; k < d.size(); ++k) {
            page = pager.word(page, word);
            word = d[k];
        }
        const auto data = pager.page(pager.word(page, word));
        out.insert(out.end(), data.begin(), data.end());
    }
    out.resize(size);
    return out;
}

inline void walk(const Pager& pager, std::uint32_t desc, const std::string& prefix,
                 std::map<std::string, std::vector<std::uint8_t>>& files) {
    const auto listing = read_file(pager, desc);
    std::size_t o = 0;
    while (o + 8 <= listing.size()) {
        const std::uint32_t target = le32(listing.data() + o);
        const std::uint16_t flags = le16(listing.data() + o + 4);
        const std::uint16_t length = le16(listing.data() + o + 6);
        o += 8;
        if (target == 0 && length == 0) break;
        if (o + length > listing.size()) throw HpfsError("directory entry name past the listing");
        // latin-1 names: one byte per character, as the Python reader decodes them
        const std::string name(reinterpret_cast<const char*>(listing.data() + o), length);
        o += length;
        if (flags & 1)
            walk(pager, target, prefix + name + "/", files);
        else
            files[prefix + name] = read_file(pager, target);
    }
}

}  // namespace detail

// {relative/path: bytes} for every file of the volume (bfhp.read_volume); directories are implied by the paths.
inline std::map<std::string, std::vector<std::uint8_t>> read_volume(const std::vector<std::uint8_t>& raw) {
    if (raw.size() < 0x20) throw HpfsError("shorter than the HPFS header");
    if (std::memcmp(raw.data(), "HPFS", 4) != 0 || detail::le32(raw.data() + 4) != kSector)
        throw HpfsError("not an HPFS volume");
    const std::uint32_t root = detail::le32(raw.data() + 0x1C);
    std::map<std::string, std::vector<std::uint8_t>> files;
    detail::walk(detail::Pager(raw), root, "", files);
    return files;
}

}  // namespace iberivm::hpfs
