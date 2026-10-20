// W4 HPFS writer without game files: synthetic trees written, checked and read back, including an empty directory,
// an empty file and files that need one level of index pages (more than 126 data pages).
#include <cstdio>
#include <string>

#include "hpfs_writer.hpp"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) ++failures;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what.c_str());
}

mapx::hpfs::Bytes pattern(std::size_t n, unsigned seed) {
    mapx::hpfs::Bytes b(n);
    for (std::size_t i = 0; i < n; ++i) b[i] = static_cast<std::uint8_t>((i * 131 + seed) >> 3);
    return b;
}

}  // namespace

int main() {
    using namespace mapx::hpfs;
    Node tree;
    tree.is_dir = true;
    put_file(tree, "game.xml", pattern(700, 1));
    put_file(tree, "empty.txt", {});
    tree.children.push_back(Node{"EmptyDir", true, {}, {}});
    put_file(tree, "Maps/1/Terrain.pass.grid", pattern(126 * kSector, 2));      // exactly level 0's capacity
    put_file(tree, "Maps/1/Terrain.decor.grid", pattern(126 * kSector + 1, 3));  // first level-1 size
    put_file(tree, "Maps/1/big.bin", pattern(300 * kSector + 17, 4));            // several index pages' worth
    put_file(tree, "game.xml", pattern(10, 5));                                  // replaced in place

    const auto vol = write_volume(tree);
    check(vol.size() % kSector == 0, "volume is whole pages");
    const auto problems = validate(vol);
    check(problems.empty(), "structural check" + (problems.empty() ? std::string() : ": " + problems.front()));

    const auto back = read_tree(vol);
    const auto files = flatten(back);
    const auto want = flatten(tree);
    check(files == want, "files read back in order");
    check(back.children.size() == 4 && back.children[0].name == "game.xml" && back.children[2].name == "EmptyDir" &&
              back.children[2].is_dir && back.children[2].children.empty(),
          "root order kept, empty directory kept");
    check(write_volume(back) == vol, "rewriting the read tree gives the same bytes");

    mapx::hpfs::Bytes broken = vol;
    broken[8] = 0xFF, broken[9] = 0xFF;  // page count beyond the file
    check(!validate(broken).empty(), "a damaged header is reported");
    check(!validate(mapx::hpfs::Bytes{'H', 'P', 'F', 'S'}).empty(), "a truncated volume is reported");

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
