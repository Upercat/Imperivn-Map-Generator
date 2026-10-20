// W8 post-build step: neutralise addresses of the original executable that header-only engine code carries in its
// error texts (for example "settlement member deleted by 0x005496E0"). Every ASCII "0x00" followed by six hex
// digits whose first lies in 4..F (the game's image range 0x00400000..0x00FFFFFF) becomes "0x00000000", same
// length, so the file layout and checksums of the sections stay valid. Run on the shipped executable only.
//   scrub_binary <file.exe>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: scrub_binary <file.exe>\n");
        return 2;
    }
    std::vector<char> b;
    {
        std::ifstream in(argv[1], std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "cannot read %s\n", argv[1]);
            return 1;
        }
        b.assign(std::istreambuf_iterator<char>(in), {});
    }
    auto hex = [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; };
    int count = 0;
    for (std::size_t i = 0; i + 10 <= b.size(); ++i) {
        if (b[i] != '0' || (b[i + 1] != 'x' && b[i + 1] != 'X') || b[i + 2] != '0' || b[i + 3] != '0') continue;
        const char first = static_cast<char>(std::toupper(static_cast<unsigned char>(b[i + 4])));
        if (!((first >= '4' && first <= '9') || (first >= 'A' && first <= 'F'))) continue;
        bool all = true;
        for (std::size_t k = 4; k < 10; ++k) all = all && hex(b[i + k]);
        if (!all || (i + 10 < b.size() && hex(b[i + 10])) || (i > 0 && std::isalnum(static_cast<unsigned char>(b[i - 1])))) continue;
        for (std::size_t k = 4; k < 10; ++k) b[i + k] = '0';
        ++count;
    }
    std::ofstream out(argv[1], std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
    if (!out) {
        std::fprintf(stderr, "cannot write %s\n", argv[1]);
        return 1;
    }
    std::printf("scrub_binary: %d address text(s) neutralised\n", count);
    return 0;
}
