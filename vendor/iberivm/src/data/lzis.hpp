// LZIS block-stream decoder (data module), the C++ twin of tools/randommap/lzis.py
// (cerebrivm format:snr-lzis-block-stream, project:lzis-offline-reader); keep the two identical.
//
// Container, read by the CCompressedFile read constructor 0x00644190 and LoadBlock 0x00643D90:
//   +0x00 "LZIS", +0x04 u32 logical size, +0x08 u32 block size, +0x0C u8 mode, +0x0D one byte not used by
//   the reader, +0x0E u32 offsets[n] with n = max(1, ceil(size / block_size)); the end of the last block is
//   the stream length. A block whose physical span is shorter than its logical size is compressed
//   (CLzisFile::DecodeBlock 0x00644170 -> LzssDecodeStream 0x00643260); otherwise its first logical-size
//   bytes are stored raw. A span longer than the block size is invalid.
//
// Block codec (LzssDecodeStream 0x00643260), bits read most-significant first:
//   - LzisReadCodeLengths 0x006425D0, twice: a 2-bit field w-2, then 286 literal/length code lengths of w
//     bits; the same again for 60 distance code lengths.
//   - LzisAssignCanonicalCodes 0x00642190: canonical codes by length, then by symbol; the code space must be
//     complete (final counter zero or a power of two).
//   - Symbols 0..255 are literals, 256..284 lengths (Deflate bases 3..258 with Deflate extra bits), 285 ends
//     the block. A distance symbol d gives base[d >> 1] + (d & 1) + 2 * extra, read with one bit fewer than
//     Deflate's code d >> 1 (Deflate distance bases 1..24577).
//   - After the end symbol the reader must be inside the last byte or exactly at the end.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace iberivm::lzis {

class LzisError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline constexpr int kLitLenSymbols = 0x11E;
inline constexpr int kDistSymbols = 0x3C;
inline constexpr int kEndSymbol = 0x11D;

// 0x007D9118 + 4*(sym-256) and 0x007D9190 + 4*(sym-256), symbols 256..285.
inline constexpr std::array<std::uint32_t, 30> kLenBase{3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258, 259};
inline constexpr std::array<int, 30> kLenExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
    4, 4, 5, 5, 5, 5, 0, 0};
// 0x007D9208 and 0x007D9288, indexed by distance symbol >> 1.
inline constexpr std::array<std::uint32_t, 30> kDistBase{1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
inline constexpr std::array<int, 30> kDistExtra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9,
    9, 10, 10, 11, 11, 12, 12, 13, 13};

namespace detail {

// MSB-first bit reader; bytes past the end read as zero (the native reader may load up to 4 bytes past the
// current one).
class Bits {
public:
    Bits(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}
    std::uint32_t peek(int n) const {
        if (n == 0) return 0;
        std::uint64_t window = 0;
        const std::size_t i = static_cast<std::size_t>(pos_ >> 3);
        for (std::size_t k = 0; k < 8; ++k) window = (window << 8) | (i + k < size_ ? data_[i + k] : 0u);
        const int shift = 64 - static_cast<int>(pos_ & 7) - n;
        return static_cast<std::uint32_t>((window >> shift) & ((std::uint64_t{1} << n) - 1));
    }
    std::uint32_t read(int n) {
        const std::uint32_t v = peek(n);
        pos_ += static_cast<std::uint64_t>(n);
        return v;
    }
    std::uint64_t pos() const { return pos_; }
    void skip(int n) { pos_ += static_cast<std::uint64_t>(n); }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::uint64_t pos_ = 0;
};

inline std::vector<int> read_lengths(Bits& bits, int count) {
    const int width = static_cast<int>(bits.read(2)) + 2;
    std::vector<int> lengths(static_cast<std::size_t>(count));
    for (int& n : lengths) n = static_cast<int>(bits.read(width));
    return lengths;
}

// Canonical prefix code (LzisAssignCanonicalCodes 0x00642190): a direct table on the next `maxlen` bits
// when maxlen <= 16, otherwise a per-length search (lengths can reach 31).
class Code {
public:
    explicit Code(const std::vector<int>& lengths) {
        maxlen_ = lengths.empty() ? 0 : *std::max_element(lengths.begin(), lengths.end());
        std::uint64_t code = 0;
        by_length_.assign(static_cast<std::size_t>(maxlen_) + 1, {});
        for (int length = 1; length <= maxlen_; ++length) {
            code <<= 1;
            for (std::size_t sym = 0; sym < lengths.size(); ++sym)
                if (lengths[sym] == length) by_length_[static_cast<std::size_t>(length)].push_back({code++, static_cast<int>(sym)});
        }
        if (code & (code - 1)) throw LzisError("incomplete prefix code");
        if (maxlen_ <= kDirect) {
            table_.assign(std::size_t{1} << maxlen_, Entry{-1, 0});
            for (int length = 1; length <= maxlen_; ++length)
                for (const auto& [c, sym] : by_length_[static_cast<std::size_t>(length)]) {
                    const std::size_t span = std::size_t{1} << (maxlen_ - length);
                    const std::size_t first = static_cast<std::size_t>(c) << (maxlen_ - length);
                    // An over-subscribed code (counter above 2^maxlen, accepted by the power-of-two check)
                    // assigns codes past the table; like the Python twin, those symbols are unreachable.
                    for (std::size_t k = 0; k < span && first + k < table_.size(); ++k) table_[first + k] = Entry{sym, length};
                }
        }
    }

    int symbol(Bits& bits) const {
        Entry entry{-1, 0};
        if (!table_.empty()) {
            entry = table_[bits.peek(maxlen_)];
        } else if (maxlen_ > kDirect) {
            for (int n = 1; n <= maxlen_ && entry.symbol < 0; ++n) {
                const std::uint32_t v = bits.peek(n);
                for (const auto& [c, sym] : by_length_[static_cast<std::size_t>(n)])
                    if (c == v) { entry = Entry{sym, n}; break; }
            }
        }
        if (entry.symbol < 0) throw LzisError("undecodable symbol");
        bits.skip(entry.length);
        return entry.symbol;
    }

private:
    static constexpr int kDirect = 16;
    struct Entry { int symbol; int length; };
    struct Assigned { std::uint64_t code; int symbol; };
    int maxlen_ = 0;
    std::vector<std::vector<Assigned>> by_length_;
    std::vector<Entry> table_;
};

}  // namespace detail

// LzssDecodeStream 0x00643260: one compressed block, at most `capacity` output bytes.
inline std::vector<std::uint8_t> decode_block(const std::uint8_t* data, std::size_t size, std::size_t capacity) {
    std::vector<std::uint8_t> out;
    if (size == 0) return out;
    if (size < 6) throw LzisError("block shorter than 6 bytes");
    detail::Bits bits(data, size);
    const auto lit_len = detail::read_lengths(bits, kLitLenSymbols);
    const auto dist_len = detail::read_lengths(bits, kDistSymbols);
    const detail::Code lit(lit_len), dist(dist_len);
    const std::uint64_t end_bits = static_cast<std::uint64_t>(size) * 8;
    int sym = lit.symbol(bits);
    while (true) {
        if (bits.pos() >= end_bits) throw LzisError("input exhausted before the end symbol");
        if (sym < 0x100) {
            if (out.size() >= capacity) throw LzisError("output buffer too small");
            out.push_back(static_cast<std::uint8_t>(sym));
        } else {
            const std::size_t k = static_cast<std::size_t>(sym - 0x100);
            const std::size_t length = kLenBase[k] + bits.read(kLenExtra[k]);
            const int d = dist.symbol(bits);
            const std::size_t j = static_cast<std::size_t>(d >> 1);
            const std::size_t distance = kDistBase[j] + static_cast<std::size_t>(d & 1) +
                2 * static_cast<std::size_t>(bits.read(std::max(kDistExtra[j] - 1, 0)));
            if (out.size() + length > capacity) throw LzisError("output buffer too small");
            if (distance > out.size()) throw LzisError("distance before the start of the block");
            const std::size_t start = out.size() - distance;
            for (std::size_t i = 0; i < length; ++i) out.push_back(out[start + i]);
        }
        sym = lit.symbol(bits);
        if (sym == kEndSymbol) break;
    }
    const std::uint64_t byte_index = bits.pos() >> 3;
    if (!(byte_index + 1 == size || (byte_index == size && (bits.pos() & 7) == 0)))
        throw LzisError("trailing data after the end symbol");
    return out;
}

inline std::vector<std::uint8_t> decode_block(const std::vector<std::uint8_t>& data, std::size_t capacity) {
    return decode_block(data.data(), data.size(), capacity);
}

struct Header {
    std::uint32_t size = 0;
    std::uint32_t block_size = 0;
    std::uint8_t mode = 0;
    std::vector<std::uint64_t> offsets;  // block starts, then the stream length
};

inline bool is_lzis(const std::vector<std::uint8_t>& raw) {
    return raw.size() >= 4 && raw[0] == 'L' && raw[1] == 'Z' && raw[2] == 'I' && raw[3] == 'S';
}

inline std::uint32_t le32(const std::vector<std::uint8_t>& raw, std::size_t at) {
    if (at + 4 > raw.size()) throw LzisError("truncated LZIS header");
    return static_cast<std::uint32_t>(raw[at]) | (static_cast<std::uint32_t>(raw[at + 1]) << 8) |
        (static_cast<std::uint32_t>(raw[at + 2]) << 16) | (static_cast<std::uint32_t>(raw[at + 3]) << 24);
}

// CCompressedFile read constructor 0x00644190: prefix and block index.
inline Header header(const std::vector<std::uint8_t>& raw) {
    if (!is_lzis(raw)) throw LzisError("not an LZIS stream");
    Header h;
    h.size = le32(raw, 4);
    h.block_size = le32(raw, 8);
    if (raw.size() < 0x0D) throw LzisError("truncated LZIS header");
    h.mode = raw[0x0C];
    if (h.size != 0 && h.block_size == 0) throw LzisError("zero block size");
    const std::uint64_t count = h.size == 0 ? 1 : (static_cast<std::uint64_t>(h.size) - 1) / h.block_size + 1;
    for (std::uint64_t i = 0; i < count; ++i) h.offsets.push_back(le32(raw, static_cast<std::size_t>(0x0E + 4 * i)));
    h.offsets.push_back(raw.size());
    return h;
}

// LoadBlock 0x00643D90 for every block: the whole logical stream.
inline std::vector<std::uint8_t> decode(const std::vector<std::uint8_t>& raw) {
    const Header h = header(raw);
    std::vector<std::uint8_t> out;
    out.reserve(h.size);
    for (std::size_t i = 0; i + 1 < h.offsets.size(); ++i) {
        // Python slice semantics: offsets clamp to the stream, a reversed span is empty.
        const std::size_t lo = static_cast<std::size_t>(std::min<std::uint64_t>(h.offsets[i], raw.size()));
        const std::size_t hi = std::max(lo, static_cast<std::size_t>(std::min<std::uint64_t>(h.offsets[i + 1], raw.size())));
        const std::size_t span = hi - lo;
        const std::uint64_t done = static_cast<std::uint64_t>(h.block_size) * i;
        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(h.block_size, h.size - done));
        if (span < want) {
            const auto block = decode_block(raw.data() + lo, span, h.block_size);
            const std::size_t take = std::min(want, block.size());
            out.insert(out.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(take));
            out.insert(out.end(), want - take, 0);
        } else if (span > h.block_size) {
            throw LzisError("block " + std::to_string(i) + ": span longer than the block size");
        } else {
            out.insert(out.end(), raw.begin() + static_cast<std::ptrdiff_t>(lo),
                raw.begin() + static_cast<std::ptrdiff_t>(lo + want));
        }
    }
    return out;
}


// Path of the shipped settlement library pack, relative to the game root (tools/randommap/data.py LIBRARY_PACK).
inline constexpr const char* kSettlementLibraryPack = "Packs/RandomMapSettlements.bfhp";

// Resolves a game-relative path component by component, case-insensitively (loose game files; the Python
// GameFS also looks into archives, but the settlement pack ships as a loose file).
inline std::filesystem::path find_game_file(const std::filesystem::path& game_root, const std::string& relative) {
    auto lower = [](std::string text) {
        for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return text;
    };
    std::filesystem::path current = game_root;
    for (const auto& part : std::filesystem::path(relative)) {
        const std::string want = lower(part.string());
        std::filesystem::path match;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(current, ec))
            if (lower(entry.path().filename().string()) == want) { match = entry.path(); break; }
        if (match.empty()) throw LzisError("game file not found: " + relative + " under " + game_root.string());
        current = match;
    }
    return current;
}

// The settlement library volume for the random-map generator: Packs/RandomMapSettlements.bfhp of the
// installed game, decoded from LZIS (an HPFS volume; tools/randommap/data.py library_volume() reads the same
// bytes before splitting them into files).
inline std::vector<std::uint8_t> read_settlement_library_volume(const std::filesystem::path& game_root) {
    const auto path = find_game_file(game_root, kSettlementLibraryPack);
    std::ifstream in(path, std::ios::binary);
    if (!in) throw LzisError("cannot open " + path.string());
    const std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return decode(raw);
}

}  // namespace iberivm::lzis
