# Source checkout validation

The self-contained source tree was clean-built on Windows 11 x64 with MSVC and CMake on
2026-10-20. The build resolves generator headers from the bundled `vendor/iberivm/src/` tree;
no other source repository or submodule is needed. The build completed without compiler warnings.

All **16 integration-enabled tests passed**, with no skips (102 seconds).
The compatible template matrix covers 19 template/size variants across eight templates.
Generated, enlarged-view and recovered exports exactly match the previously audited 0.2.3
build. The imported-header manifest was checked against Git's staged source blobs using
LF-normalized SHA-256. The bundled downloadable ZIP retains its previously validated checksum.

The complete integration run used separately configured local game data and private replay/save
fixtures. These inputs and their private manifests are not included in Git. The default CMake
configuration enables standalone checks; optional integration inputs are documented in the README.
Build caches, XML reports, screenshots, owner preferences and generated maps are excluded.

This validation does not replace pending native-game, clean-machine and multiplayer acceptance.
The version remains a beta candidate and its executable is unsigned. No license was assigned.
